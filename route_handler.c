#include "route_handler.h"

/*PRIVATE FUNCTIONS DECLARATIONS*/

//extract the url from the request body; it fills the dest buffer with valid url
static void extract_url(char *firstLineRequest,char *dest,size_t maxLen);

//extract value of specified param name if valid.
static void get_query_param(const char *url,const char* paramName,char* destBuffer,size_t maxLen);

//wrapper functions for hash table interfaces
static int handler_get(Hash_Table *table, const char *url, char *responseBuffer);
static int handler_set(Hash_Table *table, const char *url, char *responseBuffer);
static int handler_delete(Hash_Table *table, const char *url, char *responseBuffer);

//ROUTES
static Route routes[]={
    {"/get",handler_get},
    {"/set",handler_set},
    {"/delete",handler_delete},
};

/*DEFINITIONS*/

int handle_request(Hash_Table* db, char *requestBuffer, char* responseBuffer) {
    char url[URL_BUFFER_SIZE] = {0};
    char *firstLine = strtok(requestBuffer, "\n");

    if(!firstLine) {
        snprintf(responseBuffer, BUFFER_SIZE, "Bad Request\n");
        return 400;
    }
    extract_url(firstLine, url,URL_BUFFER_SIZE);

    //lookup routing
    for(int i = 0;i<sizeof(routes)/sizeof(routes[0]);i++){
        if(strncmp(url,routes[i].path,strlen(routes[i].path))== 0){
            return routes[i].handler(db,url,responseBuffer);
        }
    }
    
    //if comparation turned out bad
    snprintf(responseBuffer, BUFFER_SIZE, "route does not exist\n");
    return 404;
}

int handler_get(Hash_Table* table, const char* url, char* responseBuffer){
    char key[PARAM_KEY_SIZE] = {0};
    char* responseFromDb;
    get_query_param(url, "key=", key, PARAM_KEY_SIZE);
    if(key[0]) {
        if((responseFromDb = (char*)ht_get(table, key)) != NULL) {
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

int handler_set(Hash_Table* table, const char* url, char* responseBuffer){
    char key[PARAM_KEY_SIZE] = {0};
    char val[PARAM_VALUE_SIZE] = {0};

    get_query_param(url, "key=", key, PARAM_KEY_SIZE);
    get_query_param(url, "val=", val, PARAM_VALUE_SIZE);

    if(key[0] && val[0]) {

        if(ht_set(table, key, val, strlen(val) + 1)) {
            snprintf(responseBuffer, BUFFER_SIZE, "stored\n");
            return 200;
        } else {
            snprintf(responseBuffer, BUFFER_SIZE, "ht_set failed\n");
            return 500;
        }

    }

    snprintf(responseBuffer, BUFFER_SIZE, "Bad params\n");
    return 400;

}

int handler_delete(Hash_Table* table, const char* url, char* responseBuffer){
    char key[PARAM_KEY_SIZE] = {0};

    get_query_param(url, "key=", key, PARAM_KEY_SIZE);

    if(key[0]) {

        if(ht_delete(table, key)) {
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

static void get_query_param(const char *url,const char* paramName,char* bufferDest,size_t maxLen){
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

static void extract_url(char *firstLineRequest,char *dest,size_t maxLen){
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