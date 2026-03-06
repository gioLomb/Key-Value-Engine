#include "server.h"

int main(){
    Hash_Table *db = ht_create(5,hash_key);
    server_loop(db,start_server(PORT));
    printf("EXIT");
    ht_destroy(db);
}
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
void handle_request(Hash_Table* db, char *requestBuffer, char* responseBuffer) {
    char url[1024] = {0};
    char key[64] = {0};
    char val[64] = {0};
    char *firstLine = strtok(requestBuffer, "\n");

    if(!firstLine) {
        snprintf(responseBuffer, BUFFER_SIZE, "400 Bad Request\n");
        return;
    }
    
    extract_url(firstLine, url);
    
    // set route
    if(strncmp(url, "/set", 4) == 0) {
        get_query_params(url, "key=", key, "val=", val, NULL);
        if(key[0] && val[0]) {
            if(ht_set(db, key, val, strlen(val) + 1)) {
                snprintf(responseBuffer, BUFFER_SIZE, "200 OK: stored\n");
            } else {
                snprintf(responseBuffer, BUFFER_SIZE, "500 Internal Error: ht_set failed\n");
            }
        } else {
            snprintf(responseBuffer, BUFFER_SIZE, "400 Bad Request: missing params\n");
        }
    } 
    // get route
    else if(strncmp(url, "/get", 4) == 0) {
        char* responseFromDb;
        get_query_params(url, "key=", key, NULL);
        if(key[0]) {
            if((responseFromDb = (char*)ht_get(db, key)) != NULL) {
                snprintf(responseBuffer, BUFFER_SIZE, "200 OK: {%s}\n", responseFromDb);
            } else {
                snprintf(responseBuffer, BUFFER_SIZE, "404 Not Found: key not exists\n");
            }
        } else {
            snprintf(responseBuffer, BUFFER_SIZE, "400 Bad Request: missing key\n");
        }
    } 
    // delete route
    else if(strncmp(url, "/delete", 7) == 0) {
        get_query_params(url, "key=", key, NULL);
        if(key[0]) {
            if(ht_delete(db, key)) {
                snprintf(responseBuffer, BUFFER_SIZE, "200 OK: value deleted\n");
            } else {
                snprintf(responseBuffer, BUFFER_SIZE, "404 Not Found: key not exists\n");
            }
        } else {
            snprintf(responseBuffer, BUFFER_SIZE, "400 Bad Request: missing key\n");
        }
    } 
    else {
        snprintf(responseBuffer, BUFFER_SIZE, "404 Not Found: route does not exist\n");
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
void server_loop(Hash_Table* db,int server_fd){
    int newSocketFd;
    char requestBuffer[BUFFER_SIZE] = {0};
    char responseBuffer[BUFFER_SIZE] = {0};
    struct sockaddr_in clientAddress;
    int addrLen = sizeof(clientAddress);
    while(1) {
        // accept clients
        if ((newSocketFd = accept(server_fd, (struct sockaddr *)&clientAddress, (socklen_t*)&addrLen )) < 0) {
            perror("connection failed");
            continue;
        }

        read(newSocketFd, requestBuffer, BUFFER_SIZE);
        printf("request received\n");
        handle_request(db,requestBuffer,responseBuffer);

        close(newSocketFd);
    }
}



int start_server(int port){
    int server_fd;
    struct sockaddr_in address;

    // create tcp socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket fallito");
        exit(EXIT_FAILURE);
    }

    // configure socket
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    
    // socket options
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt fallito");
        exit(EXIT_FAILURE);
    }
    if (bind(server_fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind fallito");
        exit(EXIT_FAILURE);
    }

    //listen max 3 connections
    if (listen(server_fd, 3) < 0) {
        perror("Listen fallito");
        exit(EXIT_FAILURE);
    }

    printf("Server in ascolto sulla porta %d...\n", PORT);
    return server_fd;
}




unsigned long hash_key(const unsigned char *str,unsigned long seed) {
    unsigned long hash = seed;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    return hash;
}
