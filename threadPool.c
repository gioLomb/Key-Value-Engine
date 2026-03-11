#include "threadPool.h"

static void* worker(void *arg){

}

ThreadPool* pool_create(int threadCount, Hash_Table *db){
    ThreadPool *pool = malloc(sizeof(ThreadPool));
    if (!pool) return NULL;

    //initialize queue
    pool->queue.head = NULL;
    pool->queue.tail = NULL;
    pool->queue.size = 0;

    //initialize pthread variable
    pthread_mutex_init(&pool->mutex,NULL);
    pthread_cond_init(&pool->cond,NULL);
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
    for(int i = 0;i<threadCount;i++){
        if(pthread_create(&pool->threads[i],NULL,worker,pool) != 0){
            perror("pthread_create failed");
            pool->threadCount = i; //adapt threadCount
            pool_destroy(pool);    // cleanup
            return NULL;    
        }
    }

    return pool;
}

void pool_submit(ThreadPool *pool, int socketFd){
    Task *newTask = malloc(sizeof(Task));
    if(!newTask) return;
    
    newTask->socketFd = socketFd;
    newTask->next = NULL;

    pthread_mutex_lock(&pool->mutex);

    //add task in the queue
    if(pool->queue.head == NULL && pool->queue.tail == NULL){
        //empty queue
        pool->queue.tail = pool->queue.head = newTask;
    }else{
        pool->queue.tail->next = newTask;
        pool->queue.tail = newTask;
    }
    pool->queue.size++;

    pthread_cond_signal(&pool->cond); //run a worker
    pthread_mutex_unlock(&pool->mutex);
}

void pool_destroy(ThreadPool *pool){
    pthread_mutex_lock(&pool->mutex);

    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->cond); //active every worker
    pthread_mutex_unlock(&pool->mutex);

    //stop every thread
    for (int i = 0; i < pool->threadCount; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->cond);

    //clean up
    free(pool->threads);
    free(pool);
}