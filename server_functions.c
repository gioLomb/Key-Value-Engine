#include "hash_table.h"
#include "server_functions.h"
#include "route_handler.h"

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <fcntl.h>

volatile sig_atomic_t keep_running = 1;


/* STATIC FUNCTION PROTOTYPES */


static void handle_sigint(int sig);
static ServerCtx start_server(int port);
static int set_nonblocking(int fd);

/**
 * Creates a one-shot CLOCK_MONOTONIC timerfd armed to fire after
 * KEEPALIVE_TIMEOUT seconds. Returns the fd on success, -1 on failure.
 */
static int make_timerfd(void);

/**
 * Re-arms ctx->timer_ev.fd to KEEPALIVE_TIMEOUT seconds from now.
 * Does nothing if the fd is -1.
 */
static void reset_timer(ClientCtx *ctx);

/**
 * Unlinks ctx from the live-client list, deregisters and closes both fds,
 * frees the ClientCtx, and decrements *active_clients.
 */
static void close_client(int epoll_fd, ClientCtx *ctx,
                          ClientCtx **head, int *active_clients);

/**
 * Reads the pending HTTP request from ctx->sock_ev.fd, dispatches it to
 * handle_request(), and sends the response. Resets the timer and returns 1
 * on keep-alive, calls close_client() and returns 0 otherwise.
 */
static int dispatch_event(int epoll_fd, ClientCtx *ctx, Hash_Table *db,
                           ClientCtx **head, int *active_clients);


/* DEFINITIONS */


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
    if (ctx->timer_ev.fd == -1) return;
    struct itimerspec ts = { .it_interval = {0,0}, .it_value = {KEEPALIVE_TIMEOUT,0} };
    timerfd_settime(ctx->timer_ev.fd, 0, &ts, NULL);
}

static void close_client(int epoll_fd, ClientCtx *ctx,
                          ClientCtx **head, int *active_clients) {
    // unlink from the live-client doubly-linked list
    if (ctx->prev) ctx->prev->next = ctx->next;
    else           *head           = ctx->next;
    if (ctx->next) ctx->next->prev = ctx->prev;

    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->sock_ev.fd, NULL);
    close(ctx->sock_ev.fd);

    if (ctx->timer_ev.fd != -1) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->timer_ev.fd, NULL);
        close(ctx->timer_ev.fd);
    }

    free(ctx);
    (*active_clients)--;
}

static int dispatch_event(int epoll_fd, ClientCtx *ctx,
                           Hash_Table *db,
                           ClientCtx **head, int *active_clients) {
    char   responseBuffer[RESPONSE_BUFFER_SIZE] = {0};
    size_t totalRead = 0;
    int    keepAlive = 0;

    while (totalRead < BUFFER_SIZE - 1) {
        ssize_t nBytes = read(ctx->sock_ev.fd,
                              ctx->buffer + totalRead,
                              BUFFER_SIZE - 1 - totalRead);
        if (nBytes > 0) {
            totalRead += (size_t)nBytes;
            if (memmem(ctx->buffer, totalRead, "\r\n\r\n", 4)) break;
        } else if (nBytes == 0) {
            close_client(epoll_fd, ctx, head, active_clients);
            return 0;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (totalRead == 0) { reset_timer(ctx); return 1; }
                break;
            }
            perror("read failed");
            close_client(epoll_fd, ctx, head, active_clients);
            return 0;
        }
    }

    ctx->buffer[totalRead] = '\0';
    int statusCode = handle_request(db, ctx->buffer, responseBuffer, &keepAlive);
    send_response(ctx->sock_ev.fd, statusCode, responseBuffer, keepAlive);

    if (keepAlive) {
        reset_timer(ctx);
        return 1;
    }

    close_client(epoll_fd, ctx, head, active_clients);
    return 0;
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

    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port        = htons(port);

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

    printf("Server listening on port %d (epoll, max %d clients)...\n", port, MAX_CLIENTS);
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

void server_loop(ServerCtx sctx, Hash_Table *db) {
    struct epoll_event events[MAX_EVENTS];
    ClientCtx *head          = NULL;  // head of the live-client linked list
    int        active_clients = 0;

    while (keep_running) {
        int nReady = epoll_wait(sctx.epoll_fd, events, MAX_EVENTS, -1);
        if (nReady == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait failed");
            break;
        }

        for (int i = 0; i < nReady; i++) {

            // ── incoming connection on the listening socket ──────────────────
            if (events[i].data.fd == sctx.server_fd) {
                while (1) {
                    struct sockaddr_in clientAddr;
                    socklen_t addrLen = sizeof(clientAddr);
                    int clientFd = accept(sctx.server_fd,
                                         (struct sockaddr *)&clientAddr, &addrLen);

                    if (clientFd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept failed"); break;
                    }
                    int yes = 1;
                    setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
                    if (active_clients >= MAX_CLIENTS) {
                        fprintf(stderr, "max clients (%d) reached, dropping\n", MAX_CLIENTS);
                        close(clientFd); continue;
                    }

                    if (set_nonblocking(clientFd) == -1) {
                        perror("set_nonblocking failed");
                        close(clientFd); continue;
                    }

                    ClientCtx *ctx = calloc(1, sizeof(ClientCtx));
                    if (!ctx) {
                        fprintf(stderr, "calloc ClientCtx failed\n");
                        close(clientFd); continue;
                    }

                    int tfd = make_timerfd();
                    if (tfd == -1)
                        fprintf(stderr, "warn: make_timerfd failed (%s) - no keepalive timer\n",
                                strerror(errno));

                    // initialise the two embedded ConnectionEvents
                    ctx->sock_ev  = (ConnectionEvent){ .fd = clientFd, .type = TYPE_SOCKET, .parent = ctx };
                    ctx->timer_ev = (ConnectionEvent){ .fd = tfd,      .type = TYPE_TIMER,  .parent = ctx };

                    // register socket_fd: data.ptr points to the embedded sock_ev
                    struct epoll_event cev = { .events = EPOLLIN, .data.ptr = &ctx->sock_ev };
                    if (epoll_ctl(sctx.epoll_fd, EPOLL_CTL_ADD, clientFd, &cev) == -1) {
                        perror("epoll_ctl clientFd failed");
                        free(ctx); close(clientFd);
                        if (tfd != -1) close(tfd);
                        continue;
                    }

                    if (tfd != -1) {
                        // register timer_fd: data.ptr points to the embedded timer_ev
                        struct epoll_event tev = { .events = EPOLLIN, .data.ptr = &ctx->timer_ev };
                        if (epoll_ctl(sctx.epoll_fd, EPOLL_CTL_ADD, tfd, &tev) == -1) {
                            perror("epoll_ctl timerfd failed");
                            epoll_ctl(sctx.epoll_fd, EPOLL_CTL_DEL, clientFd, NULL);
                            free(ctx); close(clientFd); close(tfd);
                            continue;
                        }
                    }

                    // prepend to the live-client list
                    ctx->next = head;
                    ctx->prev = NULL;
                    if (head) head->prev = ctx;
                    head = ctx;

                    active_clients++;
                }
                continue;
            }

            // ── client or timer event: resolve via data.ptr ──────────────────
            // data.ptr points to a ConnectionEvent embedded inside the ClientCtx;
            // no lookup needed — type and parent are read directly
            ConnectionEvent *ev  = (ConnectionEvent *)events[i].data.ptr;
            ClientCtx       *ctx = ev->parent;

            if (ev->type == TYPE_TIMER) {
                uint64_t expirations;
                read(ctx->timer_ev.fd, &expirations, sizeof(expirations));
                close_client(sctx.epoll_fd, ctx, &head, &active_clients);
                continue;
            }

            dispatch_event(sctx.epoll_fd, ctx, db, &head, &active_clients);
        }
    }

    // graceful shutdown: walk the live-client list and close every connection
    while (head)
        close_client(sctx.epoll_fd, head, &head, &active_clients);

    close(sctx.epoll_fd);
    close(sctx.server_fd);
}

int main(int argc, char **argv) {
    int idxLoad, idxSave;
    analyze_args(argc, argv, &idxLoad, &idxSave);
    config_signal_context();

    Hash_Table *db = ht_create(HT_DEFAULT_CAPACITY, hash_key);

    if (idxLoad != -1 && ht_load(db, argv[idxLoad]))
        printf("Table loaded from %s\n", argv[idxLoad]);
    else
        printf("Starting with empty table\n");

    server_loop(start_server(PORT), db);

    ht_destroy(db, (idxSave != -1) ? argv[idxSave] : NULL);
    return 0;
}