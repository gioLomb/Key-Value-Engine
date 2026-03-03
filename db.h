#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef struct Entry{
    char *key;
    void* value;
    size_t size;
    struct Entry* next;
} Entry;

typedef struct{
    Entry **pool;
    size_t size;
    size_t capacity;
} Hash_Table;

Hash_Table* ht_create(size_t initialCapacity);
void print_table(Hash_Table *table);
int set(Hash_Table *table,char *key,void* value,size_t valueSize);
unsigned long hash_key(unsigned char *str);