#include "db.h"

unsigned long hash_key(const unsigned char *str,unsigned long seed) {
    unsigned long hash = seed;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    return hash;
}
int main(){
    Hash_Table *db = ht_create(1,hash_key);
    ht_set(db,"duce",&((int){1}),sizeof(int));
    ht_set(db,"negro",&((int){3}),sizeof(int));
    ht_set(db,"frocio",&((int){2}),sizeof(int));
    printf("%d\n",get(db,"duce",int));
    printf("%d\n",get(db,"negro",int));
    printf("%d\n",get(db,"frocio",int));

}


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
        ht_resize(table);
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

void ht_resize(Hash_Table* table){
    size_t oldCapacity = table->capacity;
    table->capacity *= 2;
    Entry **newPool = calloc(table->capacity,sizeof(Entry*));
    if(!newPool) return;

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