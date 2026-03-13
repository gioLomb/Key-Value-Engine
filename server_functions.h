/**
 * This module contains the server entry point and the supporting functions
 * that implement the startup sequence, the main accept loop, and HTTP
 * response formatting. Functions that are only called from within this
 * translation unit (signal handler, socket setup) are defined as static
 * in the .c file and are therefore not declared here.
 */

#ifndef SERVER_FUNCTIONS_H
#define SERVER_FUNCTIONS_H

#include "config.h"

// forward declaration to avoid a circular dependency with threadPool.h
typedef struct ThreadPool ThreadPool;

// set to 0 by the SIGINT handler; read by server_loop() to exit cleanly
extern volatile sig_atomic_t keep_running;

/**
 * djb2 hash function. Combines each byte of str with the accumulated hash
 * using the recurrence hash = hash * 33 + c, seeded by seed to mitigate
 * hash-flooding attacks. Matches the hash_func signature required by ht_create().
 */
unsigned long hash_key(const unsigned char *str, unsigned long seed);

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
void config_signal_context();

/**
 * Accepts connections in a loop, submitting each new socket to the thread
 * pool via pool_submit(). Exits when keep_running is cleared by SIGINT,
 * then calls pool_destroy() and closes server_fd before returning.
 */
void server_loop(ThreadPool *pool, int server_fd);

/**
 * Formats a complete HTTP/1.1 response from statusCode and responseMsg,
 * including the status line, Content-Length, and Connection headers,
 * and writes it to socketFd. Logs errors to stderr on partial or failed writes.
 */
void send_response(int socketFd, int statusCode, char *responseMsg);

#endif /* SERVER_FUNCTIONS_H */