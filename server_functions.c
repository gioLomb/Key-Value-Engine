#include "hash_table.h"
#include "server_functions.h"
#include "route_handler.h"

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <fcntl.h>

volatile sig_atomic_t keep_running = 1;


/* STATIC FUNCTION PROTOTYPES */


/**
 * Sets keep_running to 0 so that server_loop() exits cleanly on the next
 * iteration. The signal parameter is intentionally unused; the cast to void
 * suppresses the unused-parameter warning without altering behaviour.
 */
static void handle_sigint(int sig);

/**
 * Creates and configures the listening TCP socket: sets SO_REUSEADDR,
 * switches the fd to non-blocking mode, binds to INADDR_ANY on port,
 * and starts listening with a backlog of LISTEN_BACKLOG. Then creates
 * an epoll instance with EPOLL_CLOEXEC and registers the server fd for
 * edge-triggered EPOLLIN events. Calls exit(EXIT_FAILURE) on any fatal error.
 * Returns a fully initialised ServerCtx.
 */
static ServerCtx start_server(int port);

/**
 * Adds the O_NONBLOCK flag to fd using fcntl(), preserving all existing flags.
 * Returns 0 on success, -1 if either fcntl() call fails.
 */
static int set_nonblocking(int fd);

/**
 * Creates a one-shot CLOCK_MONOTONIC timerfd with TFD_NONBLOCK | TFD_CLOEXEC
 * and arms it to fire after KEEPALIVE_TIMEOUT seconds. The interval is set to
 * zero so it does not repeat. Returns the new fd on success, -1 on failure
 * (the fd is closed before returning in that case).
 */
static int make_timerfd(void);

/**
 * Re-arms the keepalive timerfd inside ctx, resetting the countdown to
 * KEEPALIVE_TIMEOUT seconds from now. Does nothing if ctx->timer_fd is -1.
 * Called after each successful request/response cycle on a keep-alive connection.
 */
static void reset_timer(ClientCtx *ctx);

/**
 * Performs a full teardown of a single client: removes both the socket fd and
 * the timerfd from epoll, deletes them from fd_map and tfd_map, closes both fds,
 * frees the heap-allocated ClientCtx, and decrements *active_clients.
 * Safe to call with ctx->timer_fd == -1 (the timer cleanup is skipped).
 */
static void close_client(int epoll_fd, ClientCtx *ctx,
                          Hash_Table *fd_map, Hash_Table *tfd_map,
                          int *active_clients);

/**
 * Reads the pending HTTP request from ctx->socket_fd into a stack buffer,
 * stopping as soon as the "\r\n\r\n" header terminator is detected or the
 * buffer is full. Dispatches the request to handle_request() and sends the
 * formatted response via send_response(). Resets the keepalive timer and
 * returns 1 if the connection should stay open, or calls close_client() and
 * returns 0 if it should be closed. Also returns 0 on read error or EOF.
 */
static int dispatch_event(int epoll_fd, ClientCtx *ctx, Hash_Table *db, Hash_Table *fd_map,
                            Hash_Table *tfd_map, int *active_clients);


/* DEFINITIONS */


unsigned long hash_key(const void *key, size_t keySize, unsigned long seed) {
    const unsigned char *bytes = (const unsigned char *)key;
    unsigned long hash = seed;
    // djb2: hash = hash * 33 + c, unrolled as (hash << 5) + hash + c
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
    // bare filename with no flag: use the same path for both load and save
    if (argc == 2 && argv[1][0] != '-') { *idxLoad = *idxSave = 1; return; }
    for (int i = 1; i < argc; i++) {
        // -ls <file>: shorthand for -l <file> -s <file>
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

static void close_client(int epoll_fd, ClientCtx *ctx,
                          Hash_Table *fd_map, Hash_Table *tfd_map,
                          int *active_clients) {
    // deregister and remove the socket fd from the bookkeeping table
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->socket_fd, NULL);
    ht_delete(fd_map, &ctx->socket_fd, sizeof(ctx->socket_fd));
    close(ctx->socket_fd);

    // deregister and remove the timerfd if one was created for this client
    if (ctx->timer_fd != -1) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->timer_fd, NULL);
        ht_delete(tfd_map, &ctx->timer_fd, sizeof(ctx->timer_fd));
        close(ctx->timer_fd);
    }

    free(ctx);
    (*active_clients)--;
}

static int dispatch_event(int epoll_fd, ClientCtx *ctx,
                           Hash_Table *db,
                           Hash_Table *fd_map, Hash_Table *tfd_map,
                           int *active_clients) {
    char   requestBuffer[BUFFER_SIZE]           = {0};
    char   responseBuffer[RESPONSE_BUFFER_SIZE] = {0};
    size_t totalRead = 0;
    int    keepAlive = 0;

    // accumulate bytes until the HTTP header terminator or buffer exhaustion
    while (totalRead < BUFFER_SIZE - 1) {
        ssize_t nBytes = read(ctx->socket_fd,
                              requestBuffer + totalRead,
                              BUFFER_SIZE - 1 - totalRead);
        if (nBytes > 0) {
            totalRead += (size_t)nBytes;
            // stop as soon as a complete HTTP header block is received
            if (memmem(requestBuffer, totalRead, "\r\n\r\n", 4)) break;
        } else if (nBytes == 0) {
            // peer closed the connection gracefully
            close_client(epoll_fd, ctx, fd_map, tfd_map, active_clients);
            return 0;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // no data available yet; if nothing was read, just refresh the timer
                if (totalRead == 0) { reset_timer(ctx); return 1; }
                break;
            }
            perror("read failed");
            close_client(epoll_fd, ctx, fd_map, tfd_map, active_clients);
            return 0;
        }
    }

    requestBuffer[totalRead] = '\0';
    int statusCode = handle_request(db, requestBuffer, responseBuffer, &keepAlive);
    send_response(ctx->socket_fd, statusCode, responseBuffer, keepAlive);

    if (keepAlive) {
        reset_timer(ctx);
        return 1;
    }

    close_client(epoll_fd, ctx, fd_map, tfd_map, active_clients);
    return 0;
}

static ServerCtx start_server(int port) {
    ServerCtx ctx;
    struct sockaddr_in address;

    ctx.server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx.server_fd == -1) { perror("socket failed"); exit(EXIT_FAILURE); }

    // allow immediate reuse of the port after a server restart
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

    // create the epoll instance and register the server fd for edge-triggered reads
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

    // fd_map: socket_fd  → ClientCtx*   (O(1) lookup on data events)
    // tfd_map: timer_fd  → ClientCtx*   (O(1) lookup on timeout events)
    Hash_Table *fd_map  = ht_create(16387, hash_key);
    Hash_Table *tfd_map = ht_create(16387, hash_key);
    if (!fd_map || !tfd_map) {
        fprintf(stderr, "ht_create failed\n");
        ht_destroy(fd_map,  NULL);
        ht_destroy(tfd_map, NULL);
        close(sctx.epoll_fd);
        close(sctx.server_fd);
        return;
    }

    int active_clients = 0;

    while (keep_running) {
        int nReady = epoll_wait(sctx.epoll_fd, events, MAX_EVENTS, -1);
        if (nReady == -1) {
            if (errno == EINTR) continue; // interrupted by signal, check keep_running
            perror("epoll_wait failed");
            break;
        }

        for (int i = 0; i < nReady; i++) {
            int fd = events[i].data.fd;

            // ── incoming connection on the listening socket ──────────────────
            if (fd == sctx.server_fd) {
                // drain all pending connections: 
                // call accept() until EAGAIN to avoid missing events
                while (1) {
                    struct sockaddr_in clientAddr;
                    socklen_t addrLen = sizeof(clientAddr);
                    int clientFd = accept(sctx.server_fd,
                                         (struct sockaddr *)&clientAddr, &addrLen);
                    if (clientFd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break; // queue drained
                        perror("accept failed"); break;
                    }

                    if (active_clients >= MAX_CLIENTS) {
                        fprintf(stderr, "max clients (%d) reached, dropping\n", MAX_CLIENTS);
                        close(clientFd); continue;
                    }

                    if (set_nonblocking(clientFd) == -1) {
                        perror("set_nonblocking failed");
                        close(clientFd); continue;
                    }

                    ClientCtx *ctx = malloc(sizeof(ClientCtx));
                    if (!ctx) {
                        fprintf(stderr, "malloc ClientCtx failed\n");
                        close(clientFd); continue;
                    }

                    int tfd = make_timerfd();
                    if (tfd == -1)
                        fprintf(stderr, "warn: make_timerfd failed (%s) — no keepalive timer\n",
                                strerror(errno));

                    ctx->socket_fd = clientFd;
                    ctx->timer_fd  = tfd;

                    // register socket_fd → ctx to resolve data events 
                    if (!ht_set(fd_map, &clientFd, sizeof(clientFd), &ctx, sizeof(ctx))) {
                        fprintf(stderr, "ht_set fd_map failed\n");
                        free(ctx); close(clientFd);
                        if (tfd != -1) close(tfd);
                        continue;
                    }

                    // register timer_fd → ctx to resolve timeout events
                    if (tfd != -1 && !ht_set(tfd_map, &tfd, sizeof(tfd), &ctx, sizeof(ctx))) {
                        fprintf(stderr, "ht_set tfd_map failed\n");
                        ht_delete(fd_map, &clientFd, sizeof(clientFd));
                        free(ctx); close(clientFd); close(tfd); continue;
                    }

                    // have client's fd monitored by epoll
                    struct epoll_event cev = { .events = EPOLLIN, .data.fd = clientFd };
                    if (epoll_ctl(sctx.epoll_fd, EPOLL_CTL_ADD, clientFd, &cev) == -1) {
                        perror("epoll_ctl clientFd failed");
                        ht_delete(fd_map, &clientFd, sizeof(clientFd));
                        if (tfd != -1) ht_delete(tfd_map, &tfd, sizeof(tfd));
                        free(ctx); close(clientFd); if (tfd != -1) close(tfd); continue;
                    }

                    if (tfd != -1) {
                        // have the timer's fd monitored by epoll
                        struct epoll_event tev = { .events = EPOLLIN, .data.fd = tfd };
                        if (epoll_ctl(sctx.epoll_fd, EPOLL_CTL_ADD, tfd, &tev) == -1) {
                            perror("epoll_ctl timerfd failed");
                            epoll_ctl(sctx.epoll_fd, EPOLL_CTL_DEL, clientFd, NULL);
                            ht_delete(fd_map,  &clientFd, sizeof(clientFd));
                            ht_delete(tfd_map, &tfd,      sizeof(tfd));
                            free(ctx); close(clientFd); close(tfd); continue;
                        }
                    }

                    active_clients++;
                }
                continue;
            }

            //keepalive timer expired
            ClientCtx *expired = NULL;
            if (ht_get(tfd_map, &fd, sizeof(fd), &expired, sizeof(expired)) && expired) {
                // consume the timerfd event to prevent it from re-firing immediately
                uint64_t expirations;
                read(fd, &expirations, sizeof(expirations));
                close_client(sctx.epoll_fd, expired, fd_map, tfd_map, &active_clients);
                continue;
            }

            //data ready on a client socket
            ClientCtx *ctx = NULL;
            if (ht_get(fd_map, &fd, sizeof(fd), &ctx, sizeof(ctx)) && ctx)
                dispatch_event(sctx.epoll_fd, ctx, db, fd_map, tfd_map, &active_clients);
        }
    }

    // graceful shutdown: close every connection still tracked in fd_map
    for (size_t i = 0; i < fd_map->capacity; i++) {
        Entry *e = fd_map->pool[i];
        while (e) {
            Entry *next = e->next;
            ClientCtx *ctx = *(ClientCtx **)e->value;
            if (ctx) {
                epoll_ctl(sctx.epoll_fd, EPOLL_CTL_DEL, ctx->socket_fd, NULL);
                if (ctx->timer_fd != -1) {
                    epoll_ctl(sctx.epoll_fd, EPOLL_CTL_DEL, ctx->timer_fd, NULL);
                    close(ctx->timer_fd);
                }
                close(ctx->socket_fd);
                free(ctx);
            }
            e = next;
        }
    }

    ht_destroy(fd_map,  NULL);
    ht_destroy(tfd_map, NULL);
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

    // persist to file on exit only if a save path was provided
    ht_destroy(db, (idxSave != -1) ? argv[idxSave] : NULL);
    return 0;
}