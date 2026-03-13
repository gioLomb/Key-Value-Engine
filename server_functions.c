#include "hash_table.h"
#include "server_functions.h"
#include "route_handler.h"
#include "threadPool.h"

volatile sig_atomic_t keep_running = 1;


// STATIC FUNCTION PROTOTYPES


// Signal handler for SIGINT (Ctrl+C): clears keep_running so server_loop
// exits cleanly on the next accept() iteration.
// Declared static because it is only registered inside config_signal_context()
// and never called directly from outside this translation unit.
static void handle_sigint(int sig);

// Creates a TCP socket, sets SO_REUSEADDR, binds it to INADDR_ANY:port,
// and starts listening. Prints an error and calls exit() on any failure.
// Returns the listening socket file descriptor.
static int start_server(int port);


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

void config_signal_context() {
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
        fprintf(stderr, "Usage:\n  %s <file>\n  %s -ls <file>\n  %s -l <file> -s <file>\n", argv[0], argv[0], argv[0]);
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

static int start_server(int port) {
    int server_fd;
    struct sockaddr_in address;

    // create TCP socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // allow immediate reuse of the port after a restart
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        exit(EXIT_FAILURE);
    }

    // bind to all interfaces on the requested port
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    if (bind(server_fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, LISTEN_BACKLOG) < 0) {
        perror("listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Server in ascolto sulla porta %d...\n", PORT);
    return server_fd;
}

void send_response(int socketFd, int statusCode, char *responseMsg,int keepAlive) {
    char *statusMsg;
    switch(statusCode) {
        case 200: statusMsg = "OK"; break;
        case 400: statusMsg = "Bad Request"; break;
        case 404: statusMsg = "Not Found"; break;
        default:  statusMsg = "Error"; break;
    }

    size_t bodyLen = strlen(responseMsg);
    size_t totalEstimatedSize = 256 + bodyLen + 1;

    char *fullResponse = malloc(totalEstimatedSize);
    if (!fullResponse) return;

    int writtenLen = snprintf(fullResponse, totalEstimatedSize,
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n\r\n"
        "%s",
        statusCode, statusMsg, bodyLen,keepAlive? "keep-alive\r\nKeep-Alive: timeout=5\r\n": "close\r\n"
        ,responseMsg);

    if (writtenLen > 0 && writtenLen < (int)totalEstimatedSize) {
        ssize_t written = write(socketFd, fullResponse, writtenLen);
        if (written < 0) perror("write failed");
        else if ((size_t)written < (size_t)writtenLen) fprintf(stderr, "partial write\n");
    }

    free(fullResponse);
}

void server_loop(ThreadPool *pool, int server_fd) {
    struct sockaddr_in clientAddress;
    socklen_t addrLen = sizeof(clientAddress);
    int newSocketFd;

    while (keep_running) {
        newSocketFd = accept(server_fd, (struct sockaddr *)&clientAddress, &addrLen);
        if (newSocketFd < 0) {
            // accept() may be interrupted by SIGINT; check the flag before reporting
            if (keep_running == 0) break;
            perror("connection failed");
            continue;
        }

        // set timeout
        struct timeval timeout = { .tv_sec = KEEPALIVE_TIMEOUT, .tv_usec = 0 };
        if (setsockopt(newSocketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
            perror("setsockopt SO_RCVTIMEO failed");
            close(newSocketFd);
            continue;
        }
        pool_submit(pool, newSocketFd);
    }

    pool_destroy(pool);
    close(server_fd);
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

    ThreadPool *pool = pool_create(8, db);
    if (!pool) {
        fprintf(stderr, "pool_create failed\n");
        ht_destroy(db, NULL);
        return EXIT_FAILURE;
    }

    server_loop(pool, start_server(PORT));

    // cleanup db
    ht_destroy(db, (idxSave != -1) ? argv[idxSave] : NULL);
    return 0;
}