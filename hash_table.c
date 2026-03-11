#include "hash_table.h"

//resize a table managing the reallocation of the entries.It returns an error code
static int ht_resize(Hash_Table* table);

int ht_get(Hash_Table *table,char *key,void *destBuffer,size_t destSize){
    pthread_rwlock_rdlock(&table->lock); //block only writers (set or delete)
    unsigned int hashedKey = table->hashFunction((unsigned char*)key,table->seed) % table->capacity;
    Entry *current = table->pool[hashedKey];
    
    while(current != NULL){
        if(strcmp(current->key,key) == 0){
            //success: fill the buffer with value
            size_t sizeToCopy = (current->size < destSize) ? current->size : destSize;
            memcpy(destBuffer,current->value,sizeToCopy);
            pthread_rwlock_unlock(&table->lock);
            return 1;
        }
        current = current->next;
    }

    pthread_rwlock_unlock(&table->lock);
    return 0;
}
int ht_set(Hash_Table *table, char *key, void* value, size_t valueSize) {
    pthread_rwlock_wrlock(&table->lock);
    unsigned long h = table->hashFunction((unsigned char*)key,table->seed); 
    unsigned int index = h % table->capacity;
    Entry *current = table->pool[index];

    //find Entry to modify it; if current is NULL means a new Entry must be created 
    while(current != NULL) {
        if(strcmp(current->key, key) == 0) {
            free(current->value);
            current->value = malloc(valueSize);
            if(current->value == NULL) goto error;
            memcpy(current->value, value, valueSize);
            current->size = valueSize;
            goto success;
        }
        current = current->next;
    }

    //check for resizing
    if(table->size + 1 >= table->capacity) {
        if(!ht_resize(table)) goto error;
        index = h % table->capacity; // compute index again
    }

    //create new Entry
    Entry *newEntry = malloc(sizeof(Entry));
    if(newEntry == NULL) goto error;

    newEntry->value = malloc(valueSize);
    if(newEntry->value == NULL) { free(newEntry); goto error; }
    memcpy(newEntry->value, value, valueSize);

    newEntry->hash = h;
    newEntry->key = strdup(key);
    newEntry->size = valueSize;
    //add on head
    newEntry->next = table->pool[index];
    table->pool[index] = newEntry;

    table->size++;
    
    success:
        pthread_rwlock_unlock(&table->lock);
        return 1;
    
    error:
        pthread_rwlock_unlock(&table->lock);
        return 0;
}

int ht_delete(Hash_Table *table,char *key){
    pthread_rwlock_wrlock(&table->lock);
    if (!table || !key) goto error; //Bad args

    unsigned int hashed_index = table->hashFunction((unsigned char*)key,table->seed) % table->capacity;
    Entry *toDelete = table->pool[hashed_index];
    Entry *prev = NULL;
    if(!toDelete) goto error;
    
    //find entry to delete
    while(toDelete != NULL && strcmp(key,toDelete->key) != 0){
        prev = toDelete;
        toDelete = toDelete->next;
    }

    if(!toDelete) goto error; //key not found

    if(!prev){
        //delete first one
        table->pool[hashed_index] = toDelete->next;
    }else{
        prev->next = toDelete->next;
    }

    //clean
    free(toDelete->key);
    free(toDelete->value);
    free(toDelete);
    table->size--;

    pthread_rwlock_unlock(&table->lock);
    return 1;
    
    error:
        pthread_rwlock_unlock(&table->lock);
        return 0;
}

Hash_Table* ht_create(size_t initialCapacity,hash_func hashFunction){
    Hash_Table *table = malloc(sizeof(Hash_Table));
    if(table == NULL) return NULL;
    
    table->size = 0;
    table->capacity = initialCapacity;


    table->pool = calloc(initialCapacity,sizeof(Entry*));
    if(table->pool == NULL){
        free(table);
        return NULL;
    }
    table->seed = generate_secure_seed();
    table->hashFunction = hashFunction;
    pthread_rwlock_init(&(table->lock),NULL);
    return table;

}

static int is_prime(size_t n) {
    if (n < 2)  return 0;
    if (n == 2 || n == 3) return 1;
    if (n % 2 == 0 || n % 3 == 0) return 0;

    //every prime > 3 follow the form 6k+1 V 6k-1
    for (size_t i = 5; i * i <= n; i += 6) {
        if (n % i == 0 || n % (i + 2) == 0) return 0;
    }
    return 1;
}

static size_t next_prime(size_t n) {
    if (n < 2) return 2;
    if (n % 2 == 0) n++;  // start from odds
    while (!is_prime(n)) n += 2;
    return n;
}

static int ht_resize(Hash_Table* table){
    size_t oldCapacity = table->capacity;
    
    //update capacity and pool
    size_t newCapacity = next_prime(table->capacity*2);
    fprintf(stderr,"new prime capacity: %zu\n",newCapacity);
    table->capacity = newCapacity;
    Entry **newPool = calloc(table->capacity,sizeof(Entry*));
    if(!newPool) return 0;

    //copy old pool's entry in the newly allocated pool 
    for(unsigned int i = 0;i<oldCapacity;i++){
        Entry *currentOldEntry = table->pool[i];
        while(currentOldEntry != NULL){
            Entry *next = currentOldEntry->next;
            unsigned int newIndex = currentOldEntry->hash %table->capacity;

            //add on head
            currentOldEntry->next = newPool[newIndex];
            newPool[newIndex] = currentOldEntry;
            currentOldEntry = next;
        }
    }
    free(table->pool);
    table->pool = newPool;
    return 1;
}

unsigned long generate_secure_seed() {
    unsigned long seed;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        fread(&seed, sizeof(seed), 1, f);
        fclose(f);
    } else {
        seed = (unsigned long)time(NULL);
    }
    return seed;
}
void ht_destroy(Hash_Table *table,const char* persistenceFilePath){
    if (!table) return;
    FILE *ptrFile=NULL;
    if(persistenceFilePath != NULL){
        ptrFile = fopen(persistenceFilePath,"wb");
        if(!ptrFile) perror("Impossible to open the file specified as persistenceFilePath parameter\n");
    }
    
    //clean and save every Entry of the pool
    for(size_t i = 0;i<table->capacity;i++){
        Entry *current = table->pool[i];

        while(current != NULL){
            Entry *next = current->next;
            if(ptrFile) save_data_on_file(current,ptrFile);
            free(current->key);
            free(current->value);
            free(current);
            current = next;
        }
    }
    if(ptrFile) fclose(ptrFile);
    free(table->pool);
    pthread_rwlock_destroy(&table->lock);
    free(table);

}

void save_data_on_file(Entry* entryToSave, FILE *f) {
    size_t key_len = strlen(entryToSave->key) + 1; 
    fwrite(&key_len, sizeof(size_t), 1, f);
    fwrite(entryToSave->key, key_len, 1, f);
    fwrite(&entryToSave->size, sizeof(size_t), 1, f);
    fwrite(entryToSave->value, entryToSave->size, 1, f);
}

int ht_load(Hash_Table *table,const char* persistenceFilePath){
    if(persistenceFilePath == NULL) return 0;
    FILE *ptrFile = fopen(persistenceFilePath,"rb");
    if(!ptrFile) return 0;  //start from empty table

    size_t keyLen,valueSize;
    char *key = NULL;
    void* value = NULL;
    //read data
    while(fread(&keyLen,sizeof(size_t),1,ptrFile) == 1){

        //read and check key 
        if(keyLen> MAX_KEY_LEN || keyLen == 0) goto error;
        key = malloc(keyLen);
        if(!key) goto error;
        if(fread(key,keyLen,1,ptrFile) != 1) goto error;

        //read and check value
        if (fread(&valueSize, sizeof(size_t), 1, ptrFile) != 1 || valueSize > MAX_VALUE_SIZE) goto error;
        value = malloc(valueSize);
        if(!value) goto error;
        if(fread(value,valueSize,1,ptrFile)!=1) goto error;

        //load data
        ht_set(table,key,value,valueSize);

        //cleanup
        free(key); key = NULL;
        free(value); value=NULL;
    }
    fclose(ptrFile);
    return 1;

    error:
        fprintf(stderr,"File corrupted or unreadable\n");
        free(key);
        free(value);
        fclose(ptrFile);
        return 0;
}