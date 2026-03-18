#include "route_handler.h"

/*PRIVATE FUNCTIONS DECLARATIONS*/

/* Returns 1 if the input contains only printable characters, decoding
   percent-encoded sequences before checking. Returns 0 otherwise.*/
static int is_sanitized(const char *input);

/* Extracts the URL path and query string from the first line of an HTTP request
   into dest. Writes an empty string if the line is malformed or the
   result would exceed maxLen. */
static void extract_url(char *firstLineRequest,char *dest,size_t maxLen);

/* Extracts the value of the specified query parameter from the URL into bufferDest.
 Writes an empty string if the parameter is not found or the value exceeds maxLen.*/
static void get_query_param(const char *url,const char* paramName,char* destBuffer,size_t maxLen);

/* wrapper functions for hash table interfaces */
static int handler_get(Hash_Table *table, const char *url, char *responseBuffer);
static int handler_set(Hash_Table *table, const char *url, char *responseBuffer);
static int handler_delete(Hash_Table *table, const char *url, char *responseBuffer);

/* ROUTES */
static Route routes[]={
    {"/get",handler_get},
    {"/set",handler_set},
    {"/delete",handler_delete},
};

/*DEFINITIONS*/

int handle_request(Hash_Table* db, char *requestBuffer, char* responseBuffer,int *keepAlive) {
    char url[URL_BUFFER_SIZE] = {0};
    
    *keepAlive = (strstr(requestBuffer, "Connection: keep-alive") != NULL) ? 1 : 0;

    //define thread local memory for thread-safety strtok
    char* saverPtr;
    char *firstLine = strtok_r(requestBuffer, "\n",&saverPtr);

    if(!firstLine) {
        snprintf(responseBuffer, BUFFER_SIZE, "Bad Request\n");
        return 400;
    }
    extract_url(firstLine, url,URL_BUFFER_SIZE);

    // iterate over the static routes array
    for(int i = 0;i<(sizeof(routes)/sizeof(routes[0]));i++){
        if(strncmp(url,routes[i].path,strlen(routes[i].path))== 0){
            return routes[i].handler(db,url,responseBuffer);
        }
    }
    
    //if comparation turned out bad
    printf("Bad routes url: %s",url);
    snprintf(responseBuffer, BUFFER_SIZE, "route does not exist:%s\n");
    return 404;
}

int handler_get(Hash_Table* table, const char* url, char* responseBuffer){
    char key[PARAM_KEY_SIZE] = {0};
    char *value = calloc(1, PARAM_VALUE_SIZE);
    if (!value) return 500;
    int statusCode;
    
    get_query_param(url, "key=", key, PARAM_KEY_SIZE);
    if(key[0]) {
        
        if(ht_get(table, key,value,PARAM_VALUE_SIZE)) {
            snprintf(responseBuffer, RESPONSE_BUFFER_SIZE, "{\"value\":\"%s\"}\n", value);
            statusCode = 200;
        } else {
            snprintf(responseBuffer, BUFFER_SIZE, "key not exists\n");
            statusCode = 404;
        }

    } else {
        snprintf(responseBuffer, BUFFER_SIZE, "missing key\n");
        statusCode = 400;;
    }

    free(value);
    return statusCode;
}

static int is_sanitized(const char *input) {
    for (const char *p = input; *p; p++) {
        char c;
        if (*p == '%' && isxdigit((unsigned char)*(p+1)) && isxdigit((unsigned char)*(p+2))) {
            //url decoding
            char hex[3] = { *(p+1), *(p+2), '\0' };
            c = (char)strtol(hex, NULL, 16);
            p += 2; 
        } else {
            c = *p;
        }
        if (!isprint((unsigned char)c)) return 0;
    }
    return 1;
}

int handler_set(Hash_Table* table, const char* url, char* responseBuffer){
    char key[PARAM_KEY_SIZE] = {0};
    char val[PARAM_VALUE_SIZE] = {0};

    get_query_param(url, "key=", key, PARAM_KEY_SIZE);
    get_query_param(url, "val=", val, PARAM_VALUE_SIZE);

    if(key[0] && val[0] && is_sanitized(key) && is_sanitized(val)) {

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

    //find the space before HTTP/x.x
    char *end = strchr(start,' ');
    if(!end) return;

    size_t len = (size_t)(end-start);
    if(len >= maxLen) return;

    memcpy(dest,start,len);
    dest[(size_t)(end-start)] = '\0';
}