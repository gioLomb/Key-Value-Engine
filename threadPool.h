/**
 * This module implements a fixed-size thread pool backed by a FIFO task queue.
 * Worker threads block on a condition variable when the queue is empty and
 * are woken up by pool_submit(). Each task carries a connected client socket
 * that the assigned worker reads, dispatches through the route handler, and
 * then closes. Shutdown is cooperative: pool_destroy() sets a flag, broadcasts
 * to all waiting workers, and joins every thread before freeing resources.
 */

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include "config.h"
#include "hash_table.h"

/**
 * Represents a single unit of work: one accepted client connection.
 * Tasks are heap-allocated and linked into a singly-linked FIFO queue;
 * the worker that dequeues a task is responsible for closing socketFd
 * and freeing the node after processing.
 */
typedef struct Task {
    int socketFd;       
    struct Task *next; 
} Task;

/**
 * A singly-linked FIFO queue of pending Task nodes.
 * New tasks are appended at tail; workers consume from head.
 * size tracks the current number of enqueued tasks and is used
 * to decide whether workers should sleep.
 */
typedef struct {
    Task *head;  
    Task *tail;  
    int   size;   
} TaskQueue;

/**
 * The thread pool container. Holds the worker thread array, the shared task
 * queue, and the synchronisation primitives that coordinate producers
 * (pool_submit) and consumers (worker threads). The shutdown flag is set
 * by pool_destroy() to signal workers to drain the queue and exit.
 * db is a shared reference to the hash table passed through to each handler.
 */
typedef struct ThreadPool{
    pthread_t *threads;     
    int threadCount;        
    TaskQueue queue;        
    pthread_mutex_t mutex;  
    pthread_cond_t cond;   
    int shutdown;           
    Hash_Table *db;         
} ThreadPool;


/* PROTOTYPES */


/**
 * Allocates and initialises a thread pool, spawning threadCount worker threads
 * that immediately begin waiting for tasks.
 * If any thread fails to start, all previously created threads are joined and
 * all resources are freed. Returns the pool pointer or NULL on failure.
 */
ThreadPool* pool_create(int threadCount, Hash_Table *db);

/**
 * Wraps socketFd in a new Task and puts it into the pool's FIFO queue,
 * then signals one waiting worker. Ownership of the socket is transferred
 * to the pool: the worker will close it after serving the request.
 * Returns without enqueuing if task allocation fails.
 */
void pool_submit(ThreadPool *pool, int socketFd);

/**
 * Initiates a graceful shutdown: sets the shutdown flag, broadcasts to all
 * workers so they can exit once the queue is drained, then joins every thread.
 * Finally destroys the mutex and condition variable and frees all pool memory.
 * Must be called exactly once; the pool pointer is invalid after this returns.
 */
void pool_destroy(ThreadPool *pool);


#endif /* THREAD_POOL_H */

