/**
 * This module contains the server entry point and the supporting functions
 * that implement the startup sequence, the epoll event loop, and HTTP
 * response formatting.
 *
 * The architecture is single-threaded and event-driven:
 *   - start_server() creates a non-blocking TCP socket and an epoll instance.
 *   - server_loop() runs epoll_wait() in a tight loop, dispatching each
 *     ready fd to either accept_connections() (server fd) or dispatch_event()
 *     (client fd), or closes idle connections when their timerfd fires.
 *   - Each accepted client gets a ClientCtx holding its socket fd and a
 *     one-shot timerfd used for the keepalive timeout. Both fds are
 *     registered in the same epoll instance, so timeout handling is
 *     integrated into the normal event loop without any extra threads.
 */

#ifndef SERVER_FUNCTIONS_H
#define SERVER_FUNCTIONS_H

#include "config.h"

// set to 0 by the SIGINT handler; read by server_loop() to exit cleanly
extern volatile sig_atomic_t keep_running;

/**
 * Holds the two file descriptors that define the server's I/O context:
 * the listening TCP socket and the epoll instance that monitors it together
 * with all accepted client sockets and their keepalive timerfds.
 */
typedef struct {
    int server_fd;
    int epoll_fd;
} ServerCtx;

/**
 * Per-connection context. socket_fd is the accepted TCP connection;
 * timer_fd is a one-shot CLOCK_MONOTONIC timerfd that fires after
 * KEEPALIVE_TIMEOUT seconds of inactivity and causes server_loop() to
 * close the connection. Both fds are registered in the epoll instance.
 * A slot with socket_fd == -1 is considered free.
 */
typedef struct {
    int socket_fd;
    int timer_fd;
} ClientCtx;

/**
 * djb2 hash function. Combines each byte of str with the accumulated hash
 * using the recurrence hash = hash * 33 + c, seeded by seed to mitigate
 * hash-flooding attacks. Matches the hash_func signature required by ht_create().
 */
unsigned long hash_key(const void *key, size_t keySize, unsigned long seed);

/**
 * Parses argv looking for flags, storing the index of the
 * load file path in idxLoad and the save file path in idxSave (-1 if absent).
 * A bare filename with no flag sets both to the same index.
 * Prints usage to stderr and exits on malformed or duplicate arguments.
 */
void analyze_args(int argc, char **argv, int *idxLoad, int *idxSave);

/**
 * Installs handle_sigint as the handler for SIGINT via sigaction.
 * Must be called once before the server loop starts.
 */
void config_signal_context(void);

/**
 * Runs the epoll event loop until keep_running is cleared by SIGINT.
 * Accepts new connections on sctx.server_fd,
 *  dispatches data events to dispatch_event(), and closes idle
 * connections when their associated timerfd fires.
 * On exit, closes all open client sockets, the epoll fd, and the server fd.
 */
void server_loop(ServerCtx sctx, Hash_Table *db);

/**
 * Formats a complete HTTP/1.1 response from statusCode and responseMsg,
 * including the status line, Content-Length, and Connection headers,
 * and writes it to socketFd. Logs errors to stderr on partial or failed writes.
 */
void send_response(int socketFd, int statusCode, char *responseMsg, int keepAlive);

#endif /* SERVER_FUNCTIONS_H */