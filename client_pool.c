#include "client_pool.h"
#include <stdlib.h>
#include <string.h>

#define CHUNK_SIZE 64

typedef struct MemoryChunk {
    ClientCtx clients[CHUNK_SIZE];
    ClientCtx *local_free_list;
    int used_count;
    struct MemoryChunk *next;
    struct MemoryChunk *prev;
} MemoryChunk;

static MemoryChunk *chunks_head = NULL;

static MemoryChunk* create_chunk(void) {
    MemoryChunk *c = calloc(1, sizeof(MemoryChunk));
    if (!c) return NULL;

    // link each client
    for (int i = 0; i < CHUNK_SIZE - 1; i++) {
        c->clients[i].next = &c->clients[i + 1];
        c->clients[i].parent_chunk = c; 
    }
    c->clients[CHUNK_SIZE - 1].next = NULL;
    c->clients[CHUNK_SIZE - 1].parent_chunk = c;

    //all clients are free at the start
    c->local_free_list = &c->clients[0];
    c->used_count = 0;
    return c;
}

int client_pool_init(void) {
    chunks_head = create_chunk();
    return (chunks_head != NULL) ? 0 : -1;
}

ClientCtx* client_pool_alloc(void) {
    MemoryChunk *curr = chunks_head;

    // find an unfull chunk
    while (curr && curr->used_count == CHUNK_SIZE) {
        curr = curr->next;
    }

    // create new chunk if noone has free space
    if (!curr) {
        curr = create_chunk();
        if (!curr) return NULL;
        // add in head
        curr->next = chunks_head;
        if (chunks_head) chunks_head->prev = curr;
        chunks_head = curr;
    }

    // extract the clientCtx object from the chunk
    ClientCtx *ctx = curr->local_free_list;
    curr->local_free_list = ctx->next; 
    curr->used_count++;

    // clean all but parent_chunk
    memset(ctx->buffer, 0, BUFFER_SIZE);
    ctx->next = ctx->prev = NULL;
    
    return ctx;
}

void client_pool_release(ClientCtx *ctx) {
    if (!ctx) return;
    MemoryChunk *c = (MemoryChunk*)ctx->parent_chunk;

    // release the object
    ctx->next = c->local_free_list;
    c->local_free_list = ctx;
    c->used_count--;

    // shrink chunk if ref count reaches 0, unless it's the last one
    if (c->used_count == 0 && (c->next || c->prev)) {
        if (c->prev) c->prev->next = c->next;
        if (c->next) c->next->prev = c->prev;
        if (c == chunks_head) chunks_head = c->next;
        free(c);
    }
}

void client_pool_destroy(void) {
    MemoryChunk *curr = chunks_head;
    while (curr) {
        MemoryChunk *next = curr->next;
        free(curr);
        curr = next;
    }
    chunks_head = NULL;
}