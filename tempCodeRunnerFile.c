int set(Hash_Table *table,char *key,void* value,size_t valueSize){
    unsigned int hashed_key = hash_key(key) % table->size;
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


}