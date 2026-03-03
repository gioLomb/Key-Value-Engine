#include "db.h"


int main(){
    Hash_Table *db = ht_create(10);
    set(db,"duce",1,sizeof(int));
    print_table(db);
}
void print_table(Hash_Table *table);
int set(Hash_Table *table,char *key,void* value,size_t valueSize){
    unsigned int hashed_key = hash_key(key) % table->capacity;
    Entry *current = table->pool[hashed_key];

    while(current != NULL){
        //check key existence
        if(strcmp(current->key,key) == 0){
            //update value
            free(current->value);
            current->value = malloc(valueSize);
            memcpy(current->value,value,valueSize);
            current->size = valueSize;
            return;
        }
        current = current->next;
    }

    //resize
    if(table->size + 1 == table->capacity){
        ht_resize(table);
        hashed_key = hash_key(key) % table->capacity;
    }

    //add new entry
    Entry *newEntry = malloc(newEntry);
    newEntry->value = malloc(valueSize);
    memcpy(newEntry->value,value,valueSize);
    strcpy(newEntry->key,key);
    newEntry->size = valueSize;
    //TO DO
}

unsigned long hash_key(unsigned char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    return hash;
}

Hash_Table* ht_create(size_t initialCapacity){
    Hash_Table *table = malloc(sizeof(Hash_Table));
    if(table == NULL) return NULL;
    
    table->size = 0;
    table->capacity = initialCapacity;


    table->pool = calloc(initialCapacity,sizeof(Entry*));
    if(table->pool == NULL){
        free(table);
        return NULL;
    }
    return table;
}