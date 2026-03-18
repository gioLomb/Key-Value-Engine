#include "hash_table.h"
#include "server_functions.h"
#include "route_handler.h"

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <fcntl.h>

volatile sig_atomic_t keep_running = 1;


// STATIC FUNCTION PROTOTYPES


// Signal handler for SIGINT (Ctrl+C): clears keep_running so the epoll
// loop exits cleanly after the current epoll_wait() returns.
static void handle_sigint(int sig);

// Creates a TCP socket, sets SO_REUSEADDR, puts it in non-blocking mode,
// binds to INADDR_ANY:port, and starts listening. Also creates the epoll
// instance and registers the listening fd with EPOLLIN. Prints an error
// and calls exit() on any failure.
// Returns a populated ServerCtx with both the server fd and the epoll fd.
static ServerCtx start_server(int port);

// Sets a file descriptor to non-blocking mode via fcntl(O_NONBLOCK).
// Returns 0 on success, -1 on failure.
static int set_nonblocking(int fd);

// Creates a CLOCK_MONOTONIC timerfd that fires once after KEEPALIVE_TIMEOUT
// seconds. Returns the timerfd fd, or -1 on failure.
static int make_timerfd(void);

// Rearms the timerfd associated with client_fd, resetting the keepalive
// countdown to KEEPALIVE_TIMEOUT seconds. Called after each successful read
// so that idle connections are closed only when truly silent.
static void reset_timer(ClientCtx *ctx);

// Looks up the ClientCtx whose timerfd matches the given tfd inside the
// client table. Returns a pointer to the entry, or NULL if not found.
// O(MAX_EVENTS) worst case — acceptable for the expected fd counts.
static ClientCtx *find_client_by_timer(ClientCtx *clients, int tfd);

// Closes both the socket fd and the timerfd for a client, removes both from
// epoll, and zeroes the ClientCtx slot so it can be reused.
static void close_client(int epoll_fd, ClientCtx *ctx);

// Core request handler for a single client event. Drains the socket with
// non-blocking read(), dispatches through handle_request(), and writes the
// response. If the client requested keep-alive the timerfd is reset;
// otherwise the connection is closed. On EAGAIN/EWOULDBLOCK the read loop
// simply stops — epoll will re-notify when more data arrives.
static void dispatch_event(int epoll_fd, ClientCtx *ctx, Hash_Table *db);


// DEFINITIONS


unsigned long hash_key(const unsigned char *str, unsigned long seed) {
    unsigned long hash = seed;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c; // djb2: hash * 33 + c
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

    // bare filename with no flag: use it for both load and save
    if (argc == 2 && argv[1][0] != '-') {
        *idxLoad = *idxSave = 1;
        return;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-ls") == 0 && (i + 1) < argc) {
            if (*idxLoad != -1 || *idxSave != -1) {
                fprintf(stderr, "Error: duplicate load/save flags\n");
                exit(EXIT_FAILURE);
            }
            *idxLoad = *idxSave = i + 1;
            return;
        }
        if (strcmp(argv[i], "-l") == 0 && (i + 1) < argc) {
            if (*idxLoad != -1) {
                fprintf(stderr, "Error: duplicate -l flag\n");
                exit(EXIT_FAILURE);
            }
            *idxLoad = i + 1;
        }
        if (strcmp(argv[i], "-s") == 0 && (i + 1) < argc) {
            if (*idxSave != -1) {
                fprintf(stderr, "Error: duplicate -s flag\n");
                exit(EXIT_FAILURE);
            }
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

    struct itimerspec ts = {
        .it_interval = { 0, 0 },               // one-shot: no repeat
        .it_value    = { KEEPALIVE_TIMEOUT, 0 } // fires after N seconds
    };
    if (timerfd_settime(tfd, 0, &ts, NULL) == -1) {
        close(tfd);
        return -1;
    }
    return tfd;
}

static void reset_timer(ClientCtx *ctx) {
    struct itimerspec ts = {
        .it_interval = { 0, 0 },
        .it_value    = { KEEPALIVE_TIMEOUT, 0 }
    };
    timerfd_settime(ctx->timer_fd, 0, &ts, NULL);
}

static ClientCtx *find_client_by_timer(ClientCtx *clients, int tfd) {
    for (int i = 0; i < MAX_EVENTS; i++) {
        if (clients[i].socket_fd != -1 && clients[i].timer_fd == tfd)
            return &clients[i];
    }
    return NULL;
}

static void close_client(int epoll_fd, ClientCtx *ctx) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->socket_fd, NULL);
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->timer_fd,  NULL);
    close(ctx->socket_fd);
    close(ctx->timer_fd);
    ctx->socket_fd = -1;
    ctx->timer_fd  = -1;
}

static ServerCtx start_server(int port) {
    ServerCtx ctx;
    struct sockaddr_in address;

    // create TCP socket
    ctx.server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx.server_fd == -1) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // allow immediate reuse of the port after a restart
    int opt = 1;
    if (setsockopt(ctx.server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR failed");
        exit(EXIT_FAILURE);
    }

    // the listening socket must be non-blocking so accept() in the event
    // loop never stalls when a connection is withdrawn before being accepted
    if (set_nonblocking(ctx.server_fd) == -1) {
        perror("set_nonblocking server_fd failed");
        exit(EXIT_FAILURE);
    }

    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port        = htons(port);
    if (bind(ctx.server_fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(ctx.server_fd, LISTEN_BACKLOG) < 0) {
        perror("listen failed");
        exit(EXIT_FAILURE);
    }

    // create the epoll instance
    ctx.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx.epoll_fd == -1) {
        perror("epoll_create1 failed");
        exit(EXIT_FAILURE);
    }

    // register the listening socket: edge-triggered so we accept() in a loop
    // until EAGAIN, draining the kernel backlog in one shot per notification
    struct epoll_event ev = {
        .events  = EPOLLET | EPOLLIN,
        .data.fd = ctx.server_fd
    };
    if (epoll_ctl(ctx.epoll_fd, EPOLL_CTL_ADD, ctx.server_fd, &ev) == -1) {
        perror("epoll_ctl server_fd failed");
        exit(EXIT_FAILURE);
    }

    printf("Server in ascolto sulla porta %d (epoll)...\n", port);
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

    size_t bodyLen           = strlen(responseMsg);
    size_t totalEstimatedSize = 256 + bodyLen + 1;

    char *fullResponse = malloc(totalEstimatedSize);
    if (!fullResponse) return;

    int writtenLen = snprintf(fullResponse, totalEstimatedSize,
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n\r\n"
        "%s",
        statusCode, statusMsg, bodyLen,
        keepAlive ? "keep-alive\r\nKeep-Alive: timeout=5" : "close",
        responseMsg);

    if (writtenLen > 0 && writtenLen < (int)totalEstimatedSize) {
        ssize_t written = write(socketFd, fullResponse, writtenLen);
        if (written < 0)
            perror("write failed");
        else if ((size_t)written < (size_t)writtenLen)
            fprintf(stderr, "partial write\n");
    }

    free(fullResponse);
}

static void dispatch_event(int epoll_fd, ClientCtx *ctx, Hash_Table *db) {
    // Accumulate chunks until we have a complete HTTP request or EAGAIN.
    // TCP is a stream protocol: a single epoll notification does not guarantee
    // that the entire request arrives in one read() call.
    char    requestBuffer[BUFFER_SIZE]           = {0};
    char    responseBuffer[RESPONSE_BUFFER_SIZE] = {0};
    size_t  totalRead = 0;
    int     keepAlive = 0;

    while (totalRead < BUFFER_SIZE - 1) {
        ssize_t nBytes = read(ctx->socket_fd,
                              requestBuffer + totalRead,
                              BUFFER_SIZE - 1 - totalRead);
        if (nBytes > 0) {
            totalRead += (size_t)nBytes;
            // blank line signals end of HTTP headers — request is complete
            if (memmem(requestBuffer, totalRead, "\r\n\r\n", 4))
                break;
        } else if (nBytes == 0) {
            // peer closed before sending a full request
            close_client(epoll_fd, ctx);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // no more data right now; if we already accumulated something
                // try to handle it, otherwise just reset the timer and wait
                if (totalRead == 0) {
                    reset_timer(ctx);
                    return;
                }
                break;
            }
            perror("read failed");
            close_client(epoll_fd, ctx);
            return;
        }
    }

    requestBuffer[totalRead] = '\0';
    int statusCode = handle_request(db, requestBuffer, responseBuffer, &keepAlive);
    send_response(ctx->socket_fd, statusCode, responseBuffer, keepAlive);

    if (keepAlive) {
        reset_timer(ctx);
    } else {
        close_client(epoll_fd, ctx);
    }
}

void server_loop(ServerCtx sctx, Hash_Table *db) {
    struct epoll_event events[MAX_EVENTS];

    // flat lookup table: index by position, socket_fd == -1 means slot is free
    ClientCtx clients[MAX_EVENTS];
    for (int i = 0; i < MAX_EVENTS; i++)
        clients[i].socket_fd = clients[i].timer_fd = -1;

    while (keep_running) {
        // block until at least one fd is ready, or SIGINT wakes us
        int nReady = epoll_wait(sctx.epoll_fd, events, MAX_EVENTS, -1);
        if (nReady == -1) {
            if (errno == EINTR) continue; // interrupted by SIGINT, recheck keep_running
            perror("epoll_wait failed");
            break;
        }

        for (int i = 0; i < nReady; i++) {
            int fd = events[i].data.fd;

            // ── new connection on the listening socket ──────────────────────
            if (fd == sctx.server_fd) {
                // EPOLLET: drain all pending connections in one shot
                while (1) {
                    struct sockaddr_in clientAddr;
                    socklen_t addrLen = sizeof(clientAddr);
                    int clientFd = accept(sctx.server_fd,
                                         (struct sockaddr *)&clientAddr,
                                         &addrLen);
                    if (clientFd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break; // no more pending connections
                        perror("accept failed");
                        break;
                    }

                    if (set_nonblocking(clientFd) == -1) {
                        perror("set_nonblocking clientFd failed");
                        close(clientFd);
                        continue;
                    }

                    // find a free ClientCtx slot
                    ClientCtx *slot = NULL;
                    for (int j = 0; j < MAX_EVENTS; j++) {
                        if (clients[j].socket_fd == -1) {
                            slot = &clients[j];
                            break;
                        }
                    }
                    if (!slot) {
                        // client table full: reject and try again next round
                        fprintf(stderr, "client table full, dropping connection\n");
                        close(clientFd);
                        continue;
                    }

                    // create a one-shot timerfd for the keepalive timeout
                    int tfd = make_timerfd();
                    if (tfd == -1) {
                        perror("make_timerfd failed");
                        close(clientFd);
                        continue;
                    }

                    slot->socket_fd = clientFd;
                    slot->timer_fd  = tfd;

                    // register the client socket: level-triggered so epoll keeps
                    // notifying us until the entire request has been read, even
                    // if the TCP stack delivers it in multiple chunks
                    struct epoll_event cev = {
                        .events  = EPOLLIN,
                        .data.fd = clientFd
                    };
                    if (epoll_ctl(sctx.epoll_fd, EPOLL_CTL_ADD, clientFd, &cev) == -1) {
                        perror("epoll_ctl clientFd failed");
                        close_client(sctx.epoll_fd, slot);
                        continue;
                    }

                    // register the keepalive timerfd
                    struct epoll_event tev = {
                        .events  = EPOLLIN,
                        .data.fd = tfd
                    };
                    if (epoll_ctl(sctx.epoll_fd, EPOLL_CTL_ADD, tfd, &tev) == -1) {
                        perror("epoll_ctl timerfd failed");
                        close_client(sctx.epoll_fd, slot);
                        continue;
                    }
                }
                continue;
            }

            // ── keepalive timer fired: idle connection expired ──────────────
            ClientCtx *expired = find_client_by_timer(clients, fd);
            if (expired) {
                // consume the timerfd read to reset its readable state
                uint64_t expirations;
                read(fd, &expirations, sizeof(expirations));
                close_client(sctx.epoll_fd, expired);
                continue;
            }

            // ── data ready on an existing client socket ─────────────────────
            for (int j = 0; j < MAX_EVENTS; j++) {
                if (clients[j].socket_fd == fd) {
                    dispatch_event(sctx.epoll_fd, &clients[j], db);
                    break;
                }
            }
        }
    }

    // graceful shutdown: close all open client connections
    for (int i = 0; i < MAX_EVENTS; i++) {
        if (clients[i].socket_fd != -1)
            close_client(sctx.epoll_fd, &clients[i]);
    }
    close(sctx.epoll_fd);
    close(sctx.server_fd);
}

int main(int argc, char **argv) {
    int idxLoad, idxSave;
    analyze_args(argc, argv, &idxLoad, &idxSave);

    config_signal_context();

    Hash_Table *db = ht_create(101, hash_key);
    if (idxLoad != -1 && ht_load(db, argv[idxLoad])) {
        printf("Table loaded from %s\n", argv[idxLoad]);
    } else {
        printf("Starting with empty table\n");
    }

    server_loop(start_server(PORT), db);

    ht_destroy(db, (idxSave != -1) ? argv[idxSave] : NULL);
    return 0;
}