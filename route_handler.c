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
    char localCopy[BUFFER_SIZE];

    strncpy(localCopy, requestBuffer, BUFFER_SIZE - 1);
    localCopy[BUFFER_SIZE - 1] = '\0';
    
    *keepAlive = (strstr(requestBuffer, "Connection: keep-alive") != NULL) ? 1 : 0;

    //define thread local memory for thread-safety strtok
    //operate on localCopy, not requestBuffer, to avoid corrupting ctx->buffer
    char* saverPtr;
    char *firstLine = strtok_r(localCopy, "\n",&saverPtr);

    if(!firstLine) {
        snprintf(responseBuffer, BUFFER_SIZE, "Bad Request\n");
        return 400;
    }
    extract_url(firstLine, url,URL_BUFFER_SIZE);

    // iterate over the static routes array
    for(size_t i = 0;i<(sizeof(routes)/sizeof(routes[0]));i++){
        size_t reqPathLen = (size_t)(strchr(url,'?')-url);
        size_t effectivePathLen = reqPathLen >= strlen(routes[i].path) ? reqPathLen : strlen(routes[i].path);
        
        if(strncmp(url,routes[i].path,effectivePathLen)== 0){
            return routes[i].handler(db,url,responseBuffer);
        }
    }
    
    //if comparation turned out bad
    printf("Bad routes url: %s",url);
    snprintf(responseBuffer, BUFFER_SIZE, "route does not exist\n");
    return 404;
}

int handler_get(Hash_Table* table, const char* url, char* responseBuffer){
    char key[PARAM_KEY_SIZE] = {0};
    char *value = calloc(1, PARAM_VALUE_SIZE);
    if (!value) return 500;
    int statusCode;
    
    get_query_param(url, "key=", key, PARAM_KEY_SIZE);
    if(key[0]) {
        
        if(ht_get(table, key,strlen(key)+1,value,PARAM_VALUE_SIZE)) {
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
    if (!input) return 0;
    
    for (const char *p = input; *p; p++) {
        if (!isprint((unsigned char)*p)) {
            return 0;
        }
    }
    return 1;
}

int handler_set(Hash_Table* table, const char* url, char* responseBuffer){
    char key[PARAM_KEY_SIZE] = {0};
    char val[PARAM_VALUE_SIZE] = {0};

    get_query_param(url, "key=", key, PARAM_KEY_SIZE);
    get_query_param(url, "val=", val, PARAM_VALUE_SIZE);

    if(key[0] && val[0] && is_sanitized(key) && is_sanitized(val)) {

        if(ht_set(table, key,strlen(key)+1, val, strlen(val) + 1)) {
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

        if(ht_delete(table, key,strlen(key)+1)) {
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

static inline int hex_to_int(char c) {
    int is_digit = (c >> 6) ^ 1;
    return (c & 0xF) + (is_digit ^ 1) * 9;
}


static void get_query_param(const char *url, const char *paramName, char *destBuffer, size_t maxLen) {
    if (!url || !paramName || !destBuffer || maxLen == 0) return;
    destBuffer[0] = '\0';

    //jump to ?
    const char *ptr = strchr(url, '?');
    if (!ptr) return;
    ptr++; 

    size_t paramLen = strlen(paramName);
    const char *found = NULL;

    // find the param key
    const char *curr = ptr;
    while ((found = strstr(curr, paramName)) != NULL) {
        //validate candidate key found
        if (found == ptr || *(found - 1) == '&') {
            ptr = found + paramLen; 
            break; 
        }
        curr = found + 1;
        found = NULL;
    }

    if (!found) return;

    size_t copied = 0;
    while (*ptr && *ptr != '&' && *ptr != ' ' && copied < (maxLen - 1)) {
        //decode hex bytes
        if (*ptr == '%' && isxdigit(ptr[1]) && isxdigit(ptr[2])) {
            int hi = hex_to_int(ptr[1]);
            int lo = hex_to_int(ptr[2]);
            
            //(hi * 16) + lo
            destBuffer[copied++] = (char)((hi << 4) | lo);
            ptr += 3; 
        } else {
            if (*ptr == '+') destBuffer[copied++] = ' ';
            else destBuffer[copied++] = *ptr;
            ptr++;
        }
    }
    destBuffer[copied] = '\0';
}

static void extract_url(char *firstLineRequest,char *dest,size_t maxLen){
    dest[0] = '\0';
    
    char *ptr = firstLineRequest;
    
    //jump to the first space
    while(*ptr && *ptr != ' ') ptr++;

    //ignores multiple spaces
    while(*ptr && *ptr == ' ') ptr++;

    //copy effective url
    size_t copied = 0;
    while (*ptr && *ptr != ' ' && *ptr != '\r' && *ptr != '\n' && copied < (maxLen - 1)) {
        dest[copied++] = *ptr++;
    }
    dest[copied] = 0;
}