#ifndef CLIENT_H
#define CLIENT_H

#include "config.h"
#include <sys/epoll.h>


struct MemoryChunk;

typedef struct {
    int fd;
    int type; 
    struct ClientCtx *parent;
} ConnectionEvent;

typedef struct ClientCtx {
    ConnectionEvent sock_ev;
    ConnectionEvent timer_ev;
    char buffer[BUFFER_SIZE];
    struct ClientCtx *next;
    struct ClientCtx *prev;
    struct MemoryChunk *parent_chunk; 
} ClientCtx;

//API
int  client_pool_init(void);
ClientCtx* client_pool_alloc(void);
void client_pool_free(ClientCtx *ctx);
void client_pool_destroy(void);

#endif