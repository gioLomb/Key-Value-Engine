#include "server.h"

int main(){
    
}

unsigned long hash_key(const unsigned char *str,unsigned long seed) {
    unsigned long hash = seed;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    return hash;
}
