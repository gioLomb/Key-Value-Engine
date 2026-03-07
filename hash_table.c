#include "hash_table.h"


void* ht_get(Hash_Table *table,char *key){
    unsigned int hashedKey = table->hashFunction((unsigned char*)key,table->seed) % table->capacity;
    Entry *current = table->pool[hashedKey];
    
    while(current != NULL){
        if(strcmp(current->key,key) == 0){
            return current->value;
        }
        current = current->next;
    }
    return NULL;
}
int ht_set(Hash_Table *table, char *key, void* value, size_t valueSize) {
    unsigned long h = table->hashFunction((unsigned char*)key,table->seed); 
    unsigned int index = h % table->capacity;
    Entry *current = table->pool[index];

    // Ricerca per UPDATE
    while(current != NULL) {
        if(strcmp(current->key, key) == 0) {
            free(current->value);
            current->value = malloc(valueSize);
            if(current->value == NULL) return 0;
            memcpy(current->value, value, valueSize);
            current->size = valueSize;
            return 1;
        }
        current = current->next;
    }

    // 2. RESIZE
    if(table->size + 1 >= table->capacity) {
        if(!ht_resize(table)) return 0;
        index = h % table->capacity; // Ricalcoliamo l'indice con l'hash che abbiamo già
    }

    
    Entry *newEntry = malloc(sizeof(Entry));
    if(newEntry == NULL) return 0;

    newEntry->value = malloc(valueSize);
    if(newEntry->value == NULL) { free(newEntry); return 0; }

    memcpy(newEntry->value, value, valueSize);
    newEntry->hash = h;
    newEntry->key = strdup(key);
    newEntry->size = valueSize;
    newEntry->next = table->pool[index];
    table->pool[index] = newEntry;

    table->size++;
    return 1;
}

int ht_delete(Hash_Table *table,char *key){
    if (!table || !key) return 0;

    unsigned int hashed_index = table->hashFunction((unsigned char*)key,table->seed) % table->capacity;
    Entry *toDelete = table->pool[hashed_index];
    Entry *prev = NULL;
    if(!toDelete) return 0;

    while(toDelete != NULL && strcmp(key,toDelete->key) != 0){
        prev = toDelete;
        toDelete = toDelete->next;
    }

    if(!toDelete) return 0; //key not found

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
    return 1;
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
    return table;

}

int ht_resize(Hash_Table* table){
    size_t oldCapacity = table->capacity;
    table->capacity *= 2;
    Entry **newPool = calloc(table->capacity,sizeof(Entry*));
    if(!newPool) return 0;

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
        seed = (unsigned long)time(NULL); // Fallback se urandom fallisce
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