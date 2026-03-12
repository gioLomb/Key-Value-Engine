#include "threadPool.h"
#include "route_handler.h"
#include "server_functions.h"

static void handle_client_task(Task *task, ThreadPool *pool) {
    char requestBuffer[BUFFER_SIZE] = {0};
    char responseBuffer[RESPONSE_BUFFER_SIZE] = {0};

    ssize_t nBytes = read(task->socketFd, requestBuffer, BUFFER_SIZE - 1);
    if (nBytes <= 0) {
        if (nBytes < 0) perror("read failed");
    } else {
        requestBuffer[nBytes] = '\0';
        int statusCode = handle_request(pool->db, requestBuffer, responseBuffer);
        send_response(task->socketFd, statusCode, responseBuffer);
    }
}

static void* worker(void *arg) {
    ThreadPool *pool = (ThreadPool*)arg;

    while (1) {
        pthread_mutex_lock(&pool->mutex);

        // wait until there is a task or shutdown
        while (pool->queue.size == 0 && !pool->shutdown) {
            //worker wait for task or shutdown
            pthread_cond_wait(&pool->cond, &pool->mutex);
        }

        // if shutdown and queue is empty, exit
        if (pool->shutdown && pool->queue.size == 0) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }

        // take task from head
        Task *task = pool->queue.head;
        pool->queue.head = task->next;
        if (pool->queue.head == NULL) pool->queue.tail = NULL;
        pool->queue.size--;

        pthread_mutex_unlock(&pool->mutex); // Rimosso il doppio unlock errato qui

        // handle client
        handle_client_task(task, pool);

        close(task->socketFd);
        free(task);
    }
}

ThreadPool* pool_create(int threadCount, Hash_Table *db) {
    ThreadPool *pool = malloc(sizeof(ThreadPool));
    if (!pool) return NULL;

    //initialize queue
    pool->queue.head = NULL;
    pool->queue.tail = NULL;
    pool->queue.size = 0;

    //initialize pthread variable
    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond, NULL);
    pool->shutdown = 0;
    pool->db = db;

    //allocate threads
    pool->threadCount = threadCount;
    pool->threads = malloc(sizeof(*pool->threads) * pool->threadCount);
    if (!pool->threads) {
        free(pool);
        return NULL;
    }

    //create the threads and run their workers
    for(int i = 0; i < threadCount; i++) {
        if(pthread_create(&pool->threads[i], NULL, worker, pool) != 0) {
            perror("pthread_create failed");
            pool->threadCount = i; //adapt threadCount
            pool_destroy(pool);    // cleanup
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

    //add task in the queue
    if(pool->queue.head == NULL && pool->queue.tail == NULL) {
        pool->queue.tail = pool->queue.head = newTask;
    } else {
        pool->queue.tail->next = newTask;
        pool->queue.tail = newTask;
    }
    pool->queue.size++;

    pthread_cond_signal(&pool->cond); 
    pthread_mutex_unlock(&pool->mutex);
}

void pool_destroy(ThreadPool *pool) {
    pthread_mutex_lock(&pool->mutex);

    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->cond); //wake up every worker
    pthread_mutex_unlock(&pool->mutex);

    // Wait for all worker threads to terminate
    for (int i = 0; i < pool->threadCount; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    //clean up
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->cond);
    free(pool->threads);
    free(pool);
}