#include "threadPool.h"
#include "route_handler.h"
#include "server_functions.h"


// STATIC FUNCTION PROTOTYPES


// Reads the HTTP request from task->socketFd, dispatches it through
// handle_request(), and writes the response back on the same socket.
// On a read error or zero-byte read the response step is skipped; the caller
// is responsible for closing the socket and freeing the task node.
static void handle_client_task(Task *task, ThreadPool *pool);

// Entry point for each worker thread. Loops indefinitely: acquires the pool
// mutex, sleeps on the condition variable while the queue is empty and shutdown
// has not been requested, dequeues the head task, releases the mutex, serves
// the client, then closes the socket and frees the task node. Exits when
// shutdown is set and the queue has been fully drained.
static void* worker(void *arg);


// DEFINITIONS


static void handle_client_task(Task *task, ThreadPool *pool) {
    int isKeptAlive = 0;
    do{
        char requestBuffer[BUFFER_SIZE] = {0};
        char responseBuffer[RESPONSE_BUFFER_SIZE] = {0};

        ssize_t nBytes = read(task->socketFd, requestBuffer, BUFFER_SIZE - 1);
        if (nBytes <= 0) {
            if (nBytes < 0){
                perror("read failed");
            }
            break;
        } else {
            requestBuffer[nBytes] = '\0';
            int statusCode = handle_request(pool->db, requestBuffer, responseBuffer,&isKeptAlive);
            send_response(task->socketFd, statusCode, responseBuffer,isKeptAlive);
        }
    }while(isKeptAlive);
}

static void* worker(void *arg) {
    ThreadPool *pool = (ThreadPool*)arg;

    while (1) {
        pthread_mutex_lock(&pool->mutex);

        // wait until there is a task or a shutdown has been requested
        while (pool->queue.size == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->cond, &pool->mutex);
        }

        // if shutdown has been requested and the queue is fully drained, exit
        if (pool->shutdown && pool->queue.size == 0) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }

        // dequeue the oldest task from the head of the queue
        Task *task = pool->queue.head;
        pool->queue.head = task->next;
        if (pool->queue.head == NULL) pool->queue.tail = NULL;
        pool->queue.size--;

        pthread_mutex_unlock(&pool->mutex);

        // serve the client, then release the socket and the task node
        handle_client_task(task, pool);
        close(task->socketFd);
        free(task);
    }
}

ThreadPool* pool_create(int threadCount, Hash_Table *db) {
    ThreadPool *pool = malloc(sizeof(ThreadPool));
    if (!pool) return NULL;

    // initialise the task queue to the empty state
    pool->queue.head = NULL;
    pool->queue.tail = NULL;
    pool->queue.size = 0;

    // initialise synchronisation primitives and shared state
    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond, NULL);
    pool->shutdown = 0;
    pool->db = db;

    // allocate the thread handle array
    pool->threadCount = threadCount;
    pool->threads = malloc(sizeof(*pool->threads) * pool->threadCount);
    if (!pool->threads) {
        free(pool);
        return NULL;
    }

    // spawn workers; on failure adjust threadCount and tear down cleanly
    for(int i = 0; i < threadCount; i++) {
        if(pthread_create(&pool->threads[i], NULL, worker, pool) != 0) {
            perror("pthread_create failed");
            pool->threadCount = i; // only join the threads that were started
            pool_destroy(pool);
            return NULL;    
        }
    }

    return pool;
}

void pool_submit(ThreadPool *pool, int socketFd) {
    Task *newTask = malloc(sizeof(Task));
    if(!newTask) return;
    
    newTask->socketFd = socketFd;
    newTask->next = NULL;

    pthread_mutex_lock(&pool->mutex);

    // append to the tail; handle both empty and non-empty queue
    if(pool->queue.head == NULL && pool->queue.tail == NULL) {
        pool->queue.tail = pool->queue.head = newTask;
    } else {
        pool->queue.tail->next = newTask;
        pool->queue.tail = newTask;
    }
    pool->queue.size++;

    // wake exactly one sleeping worker
    pthread_cond_signal(&pool->cond); 
    pthread_mutex_unlock(&pool->mutex);
}

void pool_destroy(ThreadPool *pool) {
    pthread_mutex_lock(&pool->mutex);

    // set the flag and wake every worker so they can check the exit condition
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);

    // wait for all worker threads to finish draining the queue and exit
    for (int i = 0; i < pool->threadCount; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    // release all resources
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->cond);
    free(pool->threads);
    free(pool);
}