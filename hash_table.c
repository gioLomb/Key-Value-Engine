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
void ht_destroy(Hash_Table *table){
    if (!table) return;
    for(size_t i = 0;i<table->capacity;i++){
        Entry *current = table->pool[i];

        while(current != NULL){
            Entry *next = current->next;
            free(current->key);
            free(current->value);
            free(current);
            current = next;
        }
    }
    free(table->pool);
    free(table);
}
