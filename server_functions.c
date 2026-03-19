#include "hash_table.h"
#include "server_functions.h"
#include "route_handler.h"

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <fcntl.h>

volatile sig_atomic_t keep_running = 1;

static void handle_sigint(int sig);
static ServerCtx start_server(int port);
static int set_nonblocking(int fd);
static int make_timerfd(void);
static void reset_timer(ClientCtx *ctx);
static ClientCtx *find_client_by_timer(ClientCtx *clients, int tfd);
static void close_client(int epoll_fd, ClientCtx *ctx);
static void dispatch_event(int epoll_fd, ClientCtx *ctx, Hash_Table *db);

unsigned long hash_key(const void *key, size_t keySize, unsigned long seed) {
    const unsigned char *bytes = (const unsigned char *)key;
    unsigned long hash = seed;
    for (size_t i = 0; i < keySize; i++)
        hash = ((hash << 5) + hash) + bytes[i];
    return hash;
}

static void handle_sigint(int sig) {
    (void)sig;
    printf("ctrl+c\n");
    keep_running = 0;
}

void config_signal_context(void) {
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
}

void analyze_args(int argc, char **argv, int *idxLoad, int *idxSave) {
    *idxLoad = -1;
    *idxSave = -1;
    if (argc > 5) {
        fprintf(stderr, "Usage:\n  %s <file>\n  %s -ls <file>\n  %s -l <file> -s <file>\n",
                argv[0], argv[0], argv[0]);
        exit(EXIT_FAILURE);
    }
    if (argc == 2 && argv[1][0] != '-') { *idxLoad = *idxSave = 1; return; }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-ls") == 0 && (i + 1) < argc) {
            if (*idxLoad != -1 || *idxSave != -1) {
                fprintf(stderr, "Error: duplicate load/save flags\n"); exit(EXIT_FAILURE);
            }
            *idxLoad = *idxSave = i + 1; return;
        }
        if (strcmp(argv[i], "-l") == 0 && (i + 1) < argc) {
            if (*idxLoad != -1) { fprintf(stderr, "Error: duplicate -l flag\n"); exit(EXIT_FAILURE); }
            *idxLoad = i + 1;
        }
        if (strcmp(argv[i], "-s") == 0 && (i + 1) < argc) {
            if (*idxSave != -1) { fprintf(stderr, "Error: duplicate -s flag\n"); exit(EXIT_FAILURE); }
            *idxSave = i + 1;
        }
    }
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int make_timerfd(void) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd == -1) return -1;
    struct itimerspec ts = { .it_interval = {0,0}, .it_value = {KEEPALIVE_TIMEOUT,0} };
    if (timerfd_settime(tfd, 0, &ts, NULL) == -1) { close(tfd); return -1; }
    return tfd;
}

static void reset_timer(ClientCtx *ctx) {
    if (ctx->timer_fd == -1) return;
    struct itimerspec ts = { .it_interval = {0,0}, .it_value = {KEEPALIVE_TIMEOUT,0} };
    timerfd_settime(ctx->timer_fd, 0, &ts, NULL);
}

static ClientCtx *find_client_by_timer(ClientCtx *clients, int tfd) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].socket_fd != -1 && clients[i].timer_fd == tfd)
            return &clients[i];
    return NULL;
}

static void close_client(int epoll_fd, ClientCtx *ctx) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->socket_fd, NULL);
    close(ctx->socket_fd);
    if (ctx->timer_fd != -1) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->timer_fd, NULL);
        close(ctx->timer_fd);
    }
    ctx->socket_fd = -1;
    ctx->timer_fd  = -1;
}

static ServerCtx start_server(int port) {
    ServerCtx ctx;
    struct sockaddr_in address;
    ctx.server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx.server_fd == -1) { perror("socket failed"); exit(EXIT_FAILURE); }
    int opt = 1;
    if (setsockopt(ctx.server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed"); exit(EXIT_FAILURE);
    }
    if (set_nonblocking(ctx.server_fd) == -1) { perror("set_nonblocking failed"); exit(EXIT_FAILURE); }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    if (bind(ctx.server_fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed"); exit(EXIT_FAILURE);
    }
    if (listen(ctx.server_fd, LISTEN_BACKLOG) < 0) { perror("listen failed"); exit(EXIT_FAILURE); }
    ctx.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx.epoll_fd == -1) { perror("epoll_create1 failed"); exit(EXIT_FAILURE); }
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.fd = ctx.server_fd };
    if (epoll_ctl(ctx.epoll_fd, EPOLL_CTL_ADD, ctx.server_fd, &ev) == -1) {
        perror("epoll_ctl server_fd failed"); exit(EXIT_FAILURE);
    }
    printf("Server in ascolto sulla porta %d (epoll, max %d client)...\n", port, MAX_CLIENTS);
    return ctx;
}

void send_response(int socketFd, int statusCode, char *responseMsg, int keepAlive) {
    char *statusMsg;
    switch (statusCode) {
        case 200: statusMsg = "OK";          break;
        case 400: statusMsg = "Bad Request"; break;
        case 404: statusMsg = "Not Found";   break;
        default:  statusMsg = "Error";       break;
    }
    size_t bodyLen = strlen(responseMsg);
    size_t sz = 256 + bodyLen + 1;
    char *fullResponse = malloc(sz);
    if (!fullResponse) return;
    int wlen = snprintf(fullResponse, sz,
        "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\nConnection: %s\r\n\r\n%s",
        statusCode, statusMsg, bodyLen,
        keepAlive ? "keep-alive\r\nKeep-Alive: timeout=5" : "close",
        responseMsg);
    if (wlen > 0 && wlen < (int)sz) {
        ssize_t written = write(socketFd, fullResponse, wlen);
        if (written < 0) perror("write failed");
        else if ((size_t)written < (size_t)wlen) fprintf(stderr, "partial write\n");
    }
    free(fullResponse);
}

static void dispatch_event(int epoll_fd, ClientCtx *ctx, Hash_Table *db) {
    char   requestBuffer[BUFFER_SIZE]           = {0};
    char   responseBuffer[RESPONSE_BUFFER_SIZE] = {0};
    size_t totalRead = 0;
    int    keepAlive = 0;

    while (totalRead < BUFFER_SIZE - 1) {
        ssize_t nBytes = read(ctx->socket_fd,
                              requestBuffer + totalRead,
                              BUFFER_SIZE - 1 - totalRead);
        if (nBytes > 0) {
            totalRead += (size_t)nBytes;
            if (memmem(requestBuffer, totalRead, "\r\n\r\n", 4)) break;
        } else if (nBytes == 0) {
            close_client(epoll_fd, ctx); return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (totalRead == 0) { reset_timer(ctx); return; }
                break;
            }
            perror("read failed");
            close_client(epoll_fd, ctx); return;
        }
    }

    requestBuffer[totalRead] = '\0';
    int statusCode = handle_request(db, requestBuffer, responseBuffer, &keepAlive);
    send_response(ctx->socket_fd, statusCode, responseBuffer, keepAlive);
    if (keepAlive) reset_timer(ctx);
    else           close_client(epoll_fd, ctx);
}

void server_loop(ServerCtx sctx, Hash_Table *db) {
    // MAX_EVENTS: numero massimo di eventi restituiti da epoll_wait() per chiamata.
    // Rimane piccolo (es. 1024) — e' solo un buffer di output sullo stack.
    struct epoll_event events[MAX_EVENTS];

    // MAX_CLIENTS: numero massimo di connessioni simultanee gestite.
    // Allocato sull'heap cosi' puo' essere grande (es. 16384) senza
    // rischiare stack overflow (limite tipico 8MB).
    ClientCtx *clients = calloc(MAX_CLIENTS, sizeof(ClientCtx));
    if (!clients) {
        perror("calloc clients failed");
        close(sctx.epoll_fd);
        close(sctx.server_fd);
        return;
    }
    for (int i = 0; i < MAX_CLIENTS; i++)
        clients[i].socket_fd = clients[i].timer_fd = -1;

    while (keep_running) {
        int nReady = epoll_wait(sctx.epoll_fd, events, MAX_EVENTS, -1);
        if (nReady == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait failed");
            break;
        }

        for (int i = 0; i < nReady; i++) {
            int fd = events[i].data.fd;

            // ── nuova connessione ────────────────────────────────────────────
            if (fd == sctx.server_fd) {
                while (1) {
                    struct sockaddr_in clientAddr;
                    socklen_t addrLen = sizeof(clientAddr);
                    int clientFd = accept(sctx.server_fd,
                                         (struct sockaddr *)&clientAddr, &addrLen);
                    if (clientFd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept failed"); break;
                    }
                    if (set_nonblocking(clientFd) == -1) {
                        perror("set_nonblocking clientFd failed");
                        close(clientFd); continue;
                    }
                    ClientCtx *slot = NULL;
                    for (int j = 0; j < MAX_CLIENTS; j++) {
                        if (clients[j].socket_fd == -1) { slot = &clients[j]; break; }
                    }
                    if (!slot) {
                        fprintf(stderr, "client table full (%d slots), dropping\n", MAX_CLIENTS);
                        close(clientFd); continue;
                    }
                    int tfd = make_timerfd();
                    if (tfd == -1)
                        fprintf(stderr, "warn: make_timerfd failed (%s) — no keepalive timer\n",
                                strerror(errno));
                    slot->socket_fd = clientFd;
                    slot->timer_fd  = tfd;
                    struct epoll_event cev = { .events = EPOLLIN, .data.fd = clientFd };
                    if (epoll_ctl(sctx.epoll_fd, EPOLL_CTL_ADD, clientFd, &cev) == -1) {
                        perror("epoll_ctl clientFd failed");
                        close_client(sctx.epoll_fd, slot); continue;
                    }
                    if (tfd != -1) {
                        struct epoll_event tev = { .events = EPOLLIN, .data.fd = tfd };
                        if (epoll_ctl(sctx.epoll_fd, EPOLL_CTL_ADD, tfd, &tev) == -1) {
                            perror("epoll_ctl timerfd failed");
                            close_client(sctx.epoll_fd, slot); continue;
                        }
                    }
                }
                continue;
            }

            // ── timer keepalive scaduto ──────────────────────────────────────
            ClientCtx *expired = find_client_by_timer(clients, fd);
            if (expired) {
                uint64_t expirations;
                read(fd, &expirations, sizeof(expirations));
                close_client(sctx.epoll_fd, expired);
                continue;
            }

            // ── dati pronti su un client ─────────────────────────────────────
            for (int j = 0; j < MAX_CLIENTS; j++) {
                if (clients[j].socket_fd == fd) {
                    dispatch_event(sctx.epoll_fd, &clients[j], db);
                    break;
                }
            }
        }
    }

    // shutdown: chiude tutte le connessioni aperte e libera l'heap
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].socket_fd != -1)
            close_client(sctx.epoll_fd, &clients[i]);
    free(clients);
    close(sctx.epoll_fd);
    close(sctx.server_fd);
}

int main(int argc, char **argv) {
    int idxLoad, idxSave;
    analyze_args(argc, argv, &idxLoad, &idxSave);
    config_signal_context();
    Hash_Table *db = ht_create(101, hash_key);
    if (idxLoad != -1 && ht_load(db, argv[idxLoad]))
        printf("Table loaded from %s\n", argv[idxLoad]);
    else
        printf("Starting with empty table\n");
    server_loop(start_server(PORT), db);
    ht_destroy(db, (idxSave != -1) ? argv[idxSave] : NULL);
    return 0;
}