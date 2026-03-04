#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

Hash_Table* ht_create(size_t initialCapacity,hash_func hashFunction);
void print_table(Hash_Table *table);
int ht_set(Hash_Table *table,char *key,void* value,size_t valueSize);

void* ht_get(Hash_Table *table,char *key);
void ht_resize(Hash_Table* table);
unsigned long generate_secure_seed();