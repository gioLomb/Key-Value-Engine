#include "request_handling.h"

void get_query_params(const char *url,...){
    va_list args;
    va_start(args,url);
    char* paramName;

    while((paramName = va_arg(args,char*)) != NULL){
        char* bufferDest = va_arg(args,char*);
        char *start = strstr(url,paramName);
        if(!start){
            bufferDest[0] = '\0';
            continue;
        }

        //get param value
        start+=strlen(paramName);
        char *end = strpbrk(start,"& ");
        size_t len;
        if(end){
            len = (size_t)(end-start);
        }else{
            //last param specified
            len =strlen(start);
        }

        memcpy(bufferDest,start,len);
        bufferDest[len] = '\0';
    }
        va_end(args);

}

int handle_request(Hash_Table* db, char *requestBuffer, char* responseBuffer) {
    char url[1024] = {0};
    char key[64] = {0};
    char val[64] = {0};
    char *firstLine = strtok(requestBuffer, "\n");

    if(!firstLine) {
        snprintf(responseBuffer, BUFFER_SIZE, "Bad Request\n");
        return 400;
    }
    
    extract_url(firstLine, url);
    
    // set route
    if(strncmp(url, "/set", 4) == 0) {
        get_query_params(url, "key=", key, "val=", val, NULL);
        if(key[0] && val[0]) {
            if(ht_set(db, key, val, strlen(val) + 1)) {
                snprintf(responseBuffer, BUFFER_SIZE, "stored\n");
                return 200;
            } else {
                snprintf(responseBuffer, BUFFER_SIZE, "ht_set failed\n");
                return 500;
            }
        } else {
            snprintf(responseBuffer, BUFFER_SIZE, "missing params\n");
            return 400;
        }
    } 
    // get route
    else if(strncmp(url, "/get", 4) == 0) {
        char* responseFromDb;
        get_query_params(url, "key=", key, NULL);
        if(key[0]) {
            if((responseFromDb = (char*)ht_get(db, key)) != NULL) {
                snprintf(responseBuffer, BUFFER_SIZE, "{%s}\n", responseFromDb);
                return 200;
            } else {
                snprintf(responseBuffer, BUFFER_SIZE, "key not exists\n");
                return 404;
            }
        } else {
            snprintf(responseBuffer, BUFFER_SIZE, "missing key\n");
            return 400;
        }
    } 
    // delete route
    else if(strncmp(url, "/delete", 7) == 0) {
        get_query_params(url, "key=", key, NULL);
        if(key[0]) {
            if(ht_delete(db, key)) {
                snprintf(responseBuffer, BUFFER_SIZE, "value deleted\n");
                return 200;
            } else {
                snprintf(responseBuffer, BUFFER_SIZE, "key not exists\n");
                return 404;
            }
        } else {
            snprintf(responseBuffer, BUFFER_SIZE, "missing key\n");
            return 400;
        }
    } 
    else {
        snprintf(responseBuffer, BUFFER_SIZE, "route does not exist\n");
        return 404;
    }
}

void extract_url(char *firstLineRequest,char *dest){
    dest[0] = '\0';
    char *start = strchr(firstLineRequest,'/');
    if(!start) return;

    char *end = strchr(start,' ');
    if(!end) return;

    memcpy(dest,start,(size_t)(end-start));
    dest[(size_t)(end-start)] = '\0';
}