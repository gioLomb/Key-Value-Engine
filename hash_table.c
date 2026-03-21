#include "hash_table.h"

/* STATIC FUNCTION PROTOTYPES */

/**
 * Allocates and initialises a new Entry node, performing deep copies of
 * the key and the value. The raw hash is cached in the entry to avoid recomputing
 * it during resize. Returns NULL on allocation failure.
 */
static Entry *create_entry(void *key, size_t keySize, void *value, size_t valueSize, unsigned long hash);

/**
 * Grows the bucket array to the next prime >= 2 * current capacity and relinks
 * all existing entries into the new pool without reallocating them, adapting
 * the cached hash field to avoid recomputing it. The old pool is freed after
 * relinking. Returns 1 on success, 0 if the
 * new pool allocation fails, in this case the table is left unchanged.
 */
static int ht_resize(Hash_Table *table);

/**
 * Deterministic primality test using trial division with the 6k ± 1 optimisation:
 * since all primes > 3 are of the form 6k ± 1, only those candidates are tested,
 * reducing iterations. Returns 1 if n is prime, 0 otherwise.
 */
static int is_prime(size_t n);

/**
 * Returns the smallest prime >= n, starting the search on the nearest odd number
 * to avoid testing even candidates.
 */
static size_t next_prime(size_t n);

/**
 * Serialises a single entry to an already-open binary file. Called internally
 * by ht_destroy() for each entry. Both entryToSave and file pointer must not be NULL.
 */
static inline void save_data_on_file(Entry *entryToSave, FILE *f);

/**
 * Generates a random seed by reading it from /dev/urandom,
 * falling back to time(NULL) if unavailable. Note that the fallback is deterministic
 * and should not be relied upon for security-sensitive contexts.
 */
static unsigned long generate_secure_seed(void);

/**
 * Compares two generic memory buffers for equality.
 * First checks if sizes match to avoid unnecessary memory comparison.
 * Returns 1 if buffers are identical, 0 otherwise.
 */
static inline int keys_equal(const void *a, size_t aSize, const void *b, size_t bSize);

/* API IMPLEMENTATION */


Hash_Table *ht_create(size_t initialCapacity, hash_func hashFunction) {
    Hash_Table *table = malloc(sizeof(Hash_Table));
    if (!table) return NULL;

    table->size = 0;
    table->capacity = initialCapacity > 0 ? initialCapacity : HT_DEFAULT_CAPACITY;
    table->pool = calloc(table->capacity, sizeof(Entry *));

    if (!table->pool) {
        free(table);
        return NULL;
    }

    table->seed = generate_secure_seed();
    table->hashFunction = hashFunction;
    pthread_rwlock_init(&table->lock, NULL);
    return table;
}

int ht_set(Hash_Table *table, void *key, size_t keySize, void *value, size_t valueSize) {
    pthread_rwlock_wrlock(&table->lock);

    unsigned long h = table->hashFunction(key,keySize, table->seed);
    unsigned int index = h % table->capacity;
    Entry *current = table->pool[index];

    // linear research of the specified key in the bucket chain
    while (current != NULL) {
        if (keys_equal(current->key, current->keySize, key, keySize)) {
            void *newValue = malloc(valueSize);
            if (!newValue) goto error;

            free(current->value);
            current->value = newValue;
            memcpy(current->value, value, valueSize);
            current->size = valueSize;

            pthread_rwlock_unlock(&table->lock);
            return 1;
        }
        current = current->next;
    }

    // eventually resize before inserting so the new entry lands in the right bucket
    if (table->size + 1 >= table->capacity) {
        if (!ht_resize(table)) goto error;
        index = h % table->capacity;
    }

    // prepend the new entry to the head of the bucket chain
    Entry *newEntry = create_entry(key,keySize, value, valueSize, h);
    if (!newEntry) goto error;

    newEntry->next = table->pool[index];
    table->pool[index] = newEntry;
    table->size++;

    pthread_rwlock_unlock(&table->lock);
    return 1;

error:
    pthread_rwlock_unlock(&table->lock);
    return 0;
}

int ht_get(Hash_Table *table, void *key, size_t keySize, void *destBuffer, size_t destSize) {
    pthread_rwlock_rdlock(&table->lock);

    unsigned int index = table->hashFunction(key,keySize, table->seed) % table->capacity;
    Entry *current = table->pool[index];

    while (current != NULL) {
        if (keys_equal(current->key, current->keySize, key, keySize)) {
            // copy only as many bytes as fit in the caller's buffer
            size_t sizeToCopy = (current->size < destSize) ? current->size : destSize;
            memcpy(destBuffer, current->value, sizeToCopy);
            pthread_rwlock_unlock(&table->lock);
            return 1;
        }
        current = current->next;
    }

    //key not found
    pthread_rwlock_unlock(&table->lock);
    return 0;  
}

int ht_delete(Hash_Table *table, void *key,size_t keySize) {
    if (!table || !key) return 0;
    pthread_rwlock_wrlock(&table->lock);

    unsigned int index = table->hashFunction(key,keySize, table->seed) % table->capacity;
    Entry *current = table->pool[index];
    Entry *prev = NULL;

    // scan the chain to find the deletion
    while (current != NULL && !keys_equal(current->key, current->keySize, key, keySize)) {
        prev = current;
        current = current->next;
    }

    if (!current) {
        // key not found
        pthread_rwlock_unlock(&table->lock);
        return 0;
    }

    // unlink
    if (!prev) {
        table->pool[index] = current->next;
    }else{
        prev->next = current->next;
    } 

    free(current->key);
    free(current->value);
    free(current);
    table->size--;

    pthread_rwlock_unlock(&table->lock);
    return 1;
}

void ht_destroy(Hash_Table *table, const char *persistenceFilePath) {
    if (!table) return;

    FILE *f = NULL;
    if (persistenceFilePath) {
        f = fopen(persistenceFilePath, "wb");
    }

    // walk through the bucket chains in the table
    for (size_t i = 0; i < table->capacity; i++) {
        Entry *current = table->pool[i];
        while (current) {
            Entry *next = current->next;
            if (f) save_data_on_file(current, f);
            free(current->key);
            free(current->value);
            free(current);
            current = next;
        }
    }

    if (f) fclose(f);
    free(table->pool);
    pthread_rwlock_destroy(&table->lock);
    free(table);
}

static inline void save_data_on_file(Entry *entryToSave, FILE *f) {
    fwrite(&entryToSave->keySize, sizeof(size_t), 1, f);
    fwrite(entryToSave->key, entryToSave->keySize, 1, f);
    fwrite(&entryToSave->size, sizeof(size_t), 1, f);
    fwrite(entryToSave->value, entryToSave->size, 1, f);
}

int ht_load(Hash_Table *table, const char *persistenceFilePath) {
    if (!persistenceFilePath) return 0;
    FILE *f = fopen(persistenceFilePath, "rb");
    if (!f) return 0; //starting from empty table

    char *key = NULL;
    void *val = NULL;
    size_t keyLen, valSize;

    // Each iteration reads and sets one complete record; any partial read jumps to cleanup
    while (fread(&keyLen, sizeof(size_t), 1, f) == 1) {
        key = malloc(keyLen);
        if (!key || fread(key, keyLen, 1, f) != 1) goto error;

        if (fread(&valSize, sizeof(size_t), 1, f) != 1) goto error;
        val = malloc(valSize);
        if (!val || fread(val, valSize, 1, f) != 1) goto error;

        ht_set(table, key, keyLen, val, valSize);
        free(key);
        free(val);
        key = val = NULL;
    }

    error:
        free(key);
        free(val);
        fclose(f);
        return 1;
}





static Entry *create_entry(void *key, size_t keySize, void *value, size_t valueSize, unsigned long hash) {
    Entry *newEntry = malloc(sizeof(Entry));
    if (!newEntry) return NULL;

    //prepare newEntry fields
    newEntry->key = newEntry->value = NULL;

    newEntry->key = malloc(keySize);
    if (!newEntry->key) goto error;
    memcpy(newEntry->key, key, keySize);
    newEntry->keySize = keySize;

    newEntry->value = malloc(valueSize);
    if (!newEntry->value) goto error;
    memcpy(newEntry->value, value, valueSize);
    newEntry->size = valueSize;
    newEntry->hash = hash;
    newEntry->next = NULL;

    return newEntry;

    error:
        free(newEntry->key);
        free(newEntry);
        return NULL; 
}

static int ht_resize(Hash_Table *table) {
    size_t oldCapacity = table->capacity;
    size_t newCapacity = next_prime(table->capacity * 2);

    Entry **newPool = calloc(newCapacity, sizeof(Entry *));
    if (!newPool) return 0;

    // relink every existing entry into the new pool
    for (unsigned int i = 0; i < oldCapacity; i++) {
        Entry *current = table->pool[i];
        while (current != NULL) {
            Entry *next = current->next;
            unsigned int newIndex = current->hash % newCapacity;

            // prepend to the new bucket
            current->next = newPool[newIndex];
            newPool[newIndex] = current;

            current = next;
        }
    }

    free(table->pool);
    table->pool = newPool;
    table->capacity = newCapacity;
    return 1;
}

static unsigned long generate_secure_seed(void) {
    unsigned long seed;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        fread(&seed, sizeof(seed), 1, f);
        fclose(f);
    } else {
        // fallback
        seed = (unsigned long)time(NULL);
    }
    return seed;
}

static inline int keys_equal(const void *a, size_t aSize, const void *b, size_t bSize) {
    return aSize == bSize && memcmp(a, b, aSize) == 0;
}

static int is_prime(size_t n) {
    if (n < 2) return 0;
    if (n == 2 || n == 3) return 1;
    if (n % 2 == 0 || n % 3 == 0) return 0;

    // test all integers of the form 6k ± 1
    for (size_t i = 5; i * i <= n; i += 6) {
        if (n % i == 0 || n % (i + 2) == 0) return 0;
    }
    return 1;
}

static size_t next_prime(size_t n) {
    if (n < 2) return 2;
    // Ensure we start from  an odd number
    if (n % 2 == 0) n++;
    while (!is_prime(n)) n += 2;
    return n;
}