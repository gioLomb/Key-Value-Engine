#ifndef HASH_TABLE_H
#define HASH_TABLE_H

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_KEY_LEN 1<<12
#define MAX_VALUE_SIZE 1<<20
#define get(table, key, T) (*(T*)ht_get(table, key))
typedef unsigned long (*hash_func)(const unsigned char *, unsigned long);

typedef struct Entry {
    char *key;
    void *value;
    size_t size;
    unsigned long hash;
    struct Entry *next;
} Entry;

typedef struct{
    Entry **pool;
    size_t size;
    size_t capacity;
    unsigned long seed;
    hash_func hashFunction;

} Hash_Table;

//create a new hash table with its hash function and returns it
Hash_Table* ht_create(size_t initialCapacity,hash_func hashFunction);

//clean table references
void ht_destroy(Hash_Table *table,const char* persistenceFilePath);

//set (create or modify) a value based on the key.It returns the error code
int ht_set(Hash_Table *table,char *key,void* value,size_t valueSize);

//delete a value based on the key. It returns the error code
int ht_delete(Hash_Table *table,char *key);

//get the value from a table based on the key.It returns the value
void* ht_get(Hash_Table *table,char *key);

//resize the table capacity.It returns the error code
int ht_resize(Hash_Table* table);

//load an existing table from a file.If it returns 0 the table starts as empty
int ht_load(Hash_Table *table,const char* persistenceFilePath);

//write entry in the file
void save_data_on_file(Entry* entryToSave, FILE *f);

//generate new seed to use for the hash function
unsigned long generate_secure_seed();

#endif