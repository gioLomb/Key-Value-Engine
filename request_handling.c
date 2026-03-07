#include "request_handling.h"

void get_query_param(const char *url,const char* paramName,char* bufferDest,size_t maxLen){
    char *start = strstr(url,paramName);
    bufferDest[0] = '\0';
    if(!start) return;

    //get param value
    start+=strlen(paramName);
    char *end = strpbrk(start,"& ");
    size_t len = end ? (size_t)(end-start) : strlen(start);

    if(len>=maxLen) return;
    memcpy(bufferDest,start,len);
    bufferDest[len] = '\0';
}

int handle_request(Hash_Table* db, char *requestBuffer, char* responseBuffer) {
    //TODO: sanitize key and val
    char url[URL_BUFFER_SIZE] = {0};
    char key[PARAM_KEY_SIZE] = {0}; 
    char val[PARAM_VALUE_SIZE] = {0};
    char *firstLine = strtok(requestBuffer, "\n");

    if(!firstLine) {
        snprintf(responseBuffer, BUFFER_SIZE, "Bad Request\n");
        return 400;
    }
    
    extract_url(firstLine, url,URL_BUFFER_SIZE);
    
    // set route
    if(strncmp(url, "/set", 4) == 0) {
        get_query_param(url, "key=", key, PARAM_KEY_SIZE);
        get_query_param(url, "val=", val, PARAM_VALUE_SIZE);
        if(key[0] && val[0]) {
            if(ht_set(db, key, val, strlen(val) + 1)) {
                snprintf(responseBuffer, BUFFER_SIZE, "stored\n");
                return 200;
            } else {
                snprintf(responseBuffer, BUFFER_SIZE, "ht_set failed\n");
                return 500;
            }
        } else {
            snprintf(responseBuffer, BUFFER_SIZE, "Bad params\n");
            return 400;
        }
    } 
    // get route
    else if(strncmp(url, "/get", 4) == 0) {
        char* responseFromDb;
        get_query_param(url, "key=", key, PARAM_KEY_SIZE);
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
        get_query_param(url, "key=", key, PARAM_KEY_SIZE);
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

void extract_url(char *firstLineRequest,char *dest,size_t maxLen){
    dest[0] = '\0';
    char *start = strchr(firstLineRequest,'/');
    if(!start) return;

    char *end = strchr(start,' ');
    if(!end) return;

    size_t len = (size_t)(end-start);
    if(len >= maxLen) return;

    memcpy(dest,start,len);
    dest[(size_t)(end-start)] = '\0';
}