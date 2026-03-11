#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <pthread.h>
#include "hash_table.h"


typedef struct Task {
    int socketFd;
    struct Task *next;
} Task;

typedef struct {
    Task *head;
    Task *tail;
    int   size;
} TaskQueue;

typedef struct {
    pthread_t       *threads;
    int              threadCount;
    TaskQueue        queue;
    pthread_mutex_t  mutex;
    pthread_cond_t   cond;
    int              shutdown;
    Hash_Table      *db;
} ThreadPool;

// ── public interface ───────────────────────────────────────────────────────────

// create and start the thread pool with threadCount worker threads
ThreadPool* pool_create(int threadCount, Hash_Table *db);

// submit a new client socket to the pool queue
void pool_submit(ThreadPool *pool, int socketFd);

// signal shutdown, wait for all threads to finish and free the pool
void pool_destroy(ThreadPool *pool);

#endif