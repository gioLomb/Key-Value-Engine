#ifndef CLIENT_H
#define CLIENT_H

#include "config.h"
#include <sys/epoll.h>


struct MemoryChunk;

/**
 * Describes a single monitored fd. Embedded (not heap-allocated) inside
 * ClientCtx, so &ctx->sock_ev or &ctx->timer_ev can be stored directly in
 * epoll_event.data.ptr without any extra malloc.
 * On a fired event the handler casts data.ptr to ConnectionEvent*, reads
 * type to decide the action, and follows parent to reach the owning context.
 */
typedef struct {
    struct ClientCtx *parent;
    int fd;
    int type; 
} ConnectionEvent;

/**
 * Per-connection context. sock_ev and timer_ev are embedded ConnectionEvent
 * structs registered directly in epoll; no separate allocation is needed.
 * buffer holds the incoming HTTP request for this connection.
 * next/prev link all live contexts in a doubly-linked list anchored in
 * server_loop(), enabling O(n) iteration during graceful shutdown without
 * any auxiliary data structure. Every instance belong to a parent chunk in memory
 */
typedef struct ClientCtx {
    ConnectionEvent sock_ev;
    ConnectionEvent timer_ev;
    struct ClientCtx *next;
    struct ClientCtx *prev;
    struct MemoryChunk *parent_chunk;
    char buffer[BUFFER_SIZE]; 
} ClientCtx;

//API
int  client_pool_init(void);
ClientCtx* client_pool_alloc(void);
void client_pool_release(ClientCtx *ctx);
void client_pool_destroy(void);

#endif