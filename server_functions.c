
#include "server_functions.h"

#include "route_handler.h"

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <fcntl.h>

volatile sig_atomic_t keep_running = 1;


/* STATIC FUNCTION PROTOTYPES */


/**
 * SIGINT handler: writes a short message to stdout and clears keep_running
 * so server_loop() exits cleanly on the next epoll_wait() iteration.
 * Only async-signal-safe functions are used.
 */
static void handle_sigint(int sig);

/**
 * Creates and configures the listening TCP socket (SO_REUSEADDR, non-blocking,
 * bind, listen), then creates an epoll instance and registers the server fd
 * with EPOLLIN | EPOLLET and data.ptr = NULL (sentinel for the event loop).
 * Calls exit(EXIT_FAILURE) on any fatal error. Returns a fully initialised ServerCtx.
 */
static ServerCtx start_server(int port);

/**
 * Adds O_NONBLOCK to fd via fcntl(), preserving all existing flags.
 * Returns 0 on success, -1 if either fcntl() call fails.
 */
static int set_nonblocking(int fd);

/**
 * Creates a one-shot CLOCK_MONOTONIC timerfd armed to fire after
 * KEEPALIVE_TIMEOUT seconds. Returns the fd on success, -1 on failure.
 */
static int make_timerfd(void);

/**
 * Re-arms ctx->timer_ev.fd to KEEPALIVE_TIMEOUT seconds from now.
 * Does nothing if timer_ev.fd is -1.It returns 0 on success, -1 otherwise.
 */
static int reset_timer(ClientCtx *ctx);

/**
 * Unlinks ctx from the live-client list, deregisters and closes both fds,
 * frees the ClientCtx, and decrements *active_clients.
 */
static void close_client(int epoll_fd, ClientCtx *ctx, ClientCtx **head, int *active_clients);

/**
 * Allocates and fully initialises a ClientCtx for an already-accepted clientFd.
 * Returns 1 on success, 0 on any failure (resources are released internally;
 * the caller is responsible only for closing clientFd on failure).
 */
static int setup_client(ServerCtx sctx, int clientFd, ClientCtx **head, int *active_clients);

/**
 * Drains all pending connections from the listening socket, calling
 * setup_client() for each one. Drops connections when MAX_CLIENTS is
 * reached or when setup_client() fails, logging the reason to stderr.
 */
static void accept_connections(ServerCtx sctx, ClientCtx **head, int *active_clients);

/**
 * Handles a fired timerfd event: consumes the expiration counter and closes
 * the owning client connection.
 */
static void handle_timer_event(int epoll_fd, ClientCtx *ctx, ClientCtx **head, int *active_clients);

/**
 * Reads the pending HTTP request from ctx->sock_ev.fd, dispatches it to
 * handle_request(), and sends the response. Resets the timer and returns 1
 * on keep-alive, calls close_client() and returns 0 otherwise.
 */
static int handle_socket_event(int epoll_fd, ClientCtx *ctx, Hash_Table *db, ClientCtx **head, int *active_clients);


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
    if (write(STDOUT_FILENO, "ctrl+c\n", 7) < 0) { /* nothing to do in signal handler */ }
    keep_running = 0;
}

void config_signal_context(void) {
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    //avoid crash due to write on a closed socket
    signal(SIGPIPE, SIG_IGN);
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

    //timer follows the keepalive timeout
    struct itimerspec ts = { .it_interval = {0,0}, .it_value = {KEEPALIVE_TIMEOUT,0} };
    if (timerfd_settime(tfd, 0, &ts, NULL) == -1) { close(tfd); return -1; }
    return tfd;
}

static int reset_timer(ClientCtx *ctx) {
    if (ctx->timer_ev.fd == -1) return 0; // Nessun timer, nessun problema
    struct itimerspec ts = { .it_interval = {0,0}, .it_value = {KEEPALIVE_TIMEOUT,0} };
    if (timerfd_settime(ctx->timer_ev.fd, 0, &ts, NULL) == -1) {
        perror("timerfd_settime failed");
        return -1;
    }
    return 0;
}

static void close_client(int epoll_fd, ClientCtx *ctx, ClientCtx **head, int *active_clients) {    
    //delete from list
    if (ctx->prev) ctx->prev->next = ctx->next;
    else *head = ctx->next;
    if (ctx->next) ctx->next->prev = ctx->prev;

    //delete epoll instance
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->sock_ev.fd, NULL);
    close(ctx->sock_ev.fd);

    if (ctx->timer_ev.fd != -1) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->timer_ev.fd, NULL);
        close(ctx->timer_ev.fd);
    }

    client_pool_release(ctx);
    (*active_clients)--;
}

static int setup_client(ServerCtx sctx, int clientFd, ClientCtx **head, int *active_clients) {
    ClientCtx *ctx = NULL;
    int tfd = -1;

    //set client socket's options
    if (set_nonblocking(clientFd) == -1) {
        perror("set_nonblocking failed");
        goto fail;
    }

    int yes = 1;
    setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    ctx = client_pool_alloc(); 
    if (!ctx) {
        fprintf(stderr, "Pool exhausted or allocation failed\n");
        goto fail;
    }

    tfd = make_timerfd();
    if (tfd == -1)
        fprintf(stderr, "warn: make_timerfd failed (%s) - no keepalive timer\n",
                strerror(errno));

    ctx->sock_ev = (ConnectionEvent){ .fd = clientFd, .type = TYPE_SOCKET, .parent = ctx };
    ctx->timer_ev = (ConnectionEvent){ .fd = tfd, .type = TYPE_TIMER, .parent = ctx };

    //set epoll instances
    struct epoll_event cev = { .events = EPOLLIN, .data.ptr = &ctx->sock_ev };
    if (epoll_ctl(sctx.epoll_fd, EPOLL_CTL_ADD, clientFd, &cev) == -1) {
        perror("epoll_ctl clientFd failed");
        close(clientFd);
        goto fail;
    }

    if (tfd != -1) {
        struct epoll_event tev = { .events = EPOLLIN, .data.ptr = &ctx->timer_ev };
        if (epoll_ctl(sctx.epoll_fd, EPOLL_CTL_ADD, tfd, &tev) == -1) {
            perror("epoll_ctl timerfd failed");
            epoll_ctl(sctx.epoll_fd, EPOLL_CTL_DEL, clientFd, NULL);
            goto fail;
        }
    }

    //add to head
    ctx->next = *head;
    ctx->prev = NULL;
    if (*head) (*head)->prev = ctx;
    *head = ctx;
    (*active_clients)++;
    return 1;

fail:
    if (clientFd != -1) close(clientFd); 
    if (tfd != -1) close(tfd);
    free(ctx);
    return 0;
}

static void accept_connections(ServerCtx sctx, ClientCtx **head, int *active_clients) {
    while (1) {
        int clientFd = accept(sctx.server_fd, NULL, NULL);
        if (clientFd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("accept failed");
            break;
        }

        if (*active_clients >= MAX_CLIENTS) {
            fprintf(stderr, "max clients (%d) reached, dropping\n", MAX_CLIENTS);
            close(clientFd);
            continue;
        }

        if (!setup_client(sctx, clientFd, head, active_clients))
            close(clientFd);
    }
}

static void handle_timer_event(int epoll_fd, ClientCtx *ctx, ClientCtx **head, int *active_clients) {
    // consume the expiration counter so the fd does not re-fire immediately
    uint64_t expirations;
    if (read(ctx->timer_ev.fd, &expirations, sizeof(expirations)) == -1 && errno != EAGAIN)
        perror("timerfd read failed");

    close_client(epoll_fd, ctx, head, active_clients);
}

static int handle_socket_event(int epoll_fd, ClientCtx *ctx, Hash_Table *db,
                                ClientCtx **head, int *active_clients) {
    char   responseBuffer[RESPONSE_BUFFER_SIZE] = {0};
    size_t totalRead = 0;
    int    keepAlive = 0;
    
    //get bytes from client (until EAGAIN)
    while (totalRead < BUFFER_SIZE - 1) {
        ssize_t nBytes = read(ctx->sock_ev.fd, ctx->buffer + totalRead, BUFFER_SIZE - 1 - totalRead);

        if (nBytes > 0) {
            totalRead += (size_t)nBytes;
            if (memmem(ctx->buffer, totalRead, "\r\n\r\n", 4)) break;
        } else if (nBytes == 0) {
            // peer closed the connection
            goto close_connection;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (totalRead == 0) { 
                    if(reset_timer(ctx) == -1) goto close_connection; 
                    return 1; 
                }
                break;
            }
            perror("read failed");
            goto close_connection;
        }
    }

    ctx->buffer[totalRead] = '\0';
    int statusCode = handle_request(db, ctx->buffer, responseBuffer, &keepAlive);
    send_response(ctx->sock_ev.fd, statusCode, responseBuffer, keepAlive);

    if (keepAlive) {
        if(reset_timer(ctx) == -1) goto close_connection; 
        return 1;
    }

    close_connection:
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

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(ctx.server_fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed"); exit(EXIT_FAILURE);
    }
    if (listen(ctx.server_fd, LISTEN_BACKLOG) < 0) { perror("listen failed"); exit(EXIT_FAILURE); }

    ctx.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx.epoll_fd == -1) { perror("epoll_create1 failed"); exit(EXIT_FAILURE); }

    // server fd uses data.ptr = NULL as sentinel ,distinguishable from any
    // valid ConnectionEvent pointer in the event loop
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.ptr = NULL };
    if (epoll_ctl(ctx.epoll_fd, EPOLL_CTL_ADD, ctx.server_fd, &ev) == -1) {
        perror("epoll_ctl server_fd failed"); exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d (epoll, max %d clients)...\n", port, MAX_CLIENTS);
    return ctx;
}

void send_response(int socketFd, int statusCode, char *responseMsg, int keepAlive) {
    static char fullResponse[256 + RESPONSE_BUFFER_SIZE];
    char *statusMsg;
    switch (statusCode) {
        case 200: statusMsg = "OK"; break;
        case 400: statusMsg = "Bad Request"; break;
        case 404: statusMsg = "Not Found"; break;
        default:  statusMsg = "Error"; break;
    }

    //make response string
    int wlen = snprintf(fullResponse, sizeof(fullResponse),
        "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\nConnection: %s\r\n\r\n%s",
        statusCode, statusMsg, strlen(responseMsg),
        keepAlive ? "keep-alive\r\nKeep-Alive: timeout=5" : "close",
        responseMsg);

    //write response
    if (wlen > 0 && wlen < (int)sizeof(fullResponse)) {
        ssize_t written = write(socketFd, fullResponse, wlen);
        if (written < 0) perror("write failed");
        else if ((size_t)written < (size_t)wlen) fprintf(stderr, "partial write\n");
    }
}

void server_loop(ServerCtx sctx, Hash_Table *db) {
    struct epoll_event events[MAX_EVENTS];
    ClientCtx *head = NULL;
    int active_clients = 0;

    while (keep_running) {
        int nReady = epoll_wait(sctx.epoll_fd, events, MAX_EVENTS, -1);
        if (nReady == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait failed");
            break;
        }

        for (int i = 0; i < nReady; i++) {
            ConnectionEvent *ev = (ConnectionEvent *)events[i].data.ptr;

            // NULL sentinel for server event
            if (ev == NULL) {
                accept_connections(sctx, &head, &active_clients);
                continue;
            }

            ClientCtx *ctx = ev->parent;

            if (ev->type == TYPE_TIMER) {
                handle_timer_event(sctx.epoll_fd, ctx, &head, &active_clients);
                continue;
            }

            handle_socket_event(sctx.epoll_fd, ctx, db, &head, &active_clients);
        }
    }

    // shutdown: walk the live-client list and close every connection
    while (head)
        close_client(sctx.epoll_fd, head, &head, &active_clients);

    close(sctx.epoll_fd);
    close(sctx.server_fd);
}

int main(int argc, char **argv) {
    int idxLoad, idxSave;
    analyze_args(argc, argv, &idxLoad, &idxSave);
    config_signal_context();

    if (client_pool_init() == -1) {
        fprintf(stderr, "Critical: Could not initialize client pool\n");
        return EXIT_FAILURE;
    }

    Hash_Table *db = ht_create(16384, hash_key);

    if (idxLoad != -1 && ht_load(db, argv[idxLoad]))
        printf("Table loaded from %s\n", argv[idxLoad]);
    else
        printf("Starting with empty table\n");

    server_loop(start_server(PORT), db);

    //clean
    client_pool_destroy();
    ht_destroy(db, (idxSave != -1) ? argv[idxSave] : NULL);
    return 0;
}