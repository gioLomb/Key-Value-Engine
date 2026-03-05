#include "server.h"

int main(){
    Hash_Table *db = ht_create(5,hash_key);
    server_loop(db,start_server(PORT));
    printf("EXIT");
    ht_destroy(db);
}
void get_query_params(const char *url,...){//TODO
    va_list args;
    va_start(args,url);

}
void handle_request(Hash_Table* db,char *buffer){
    //TODO: improve error handling? , response sendind instead of printf
    char url[1024] = {0};
    char key[64] = {0};
    char val[64] = {0};
    char* response;
    char *firstLine = strtok(buffer,"\n");

    if(!firstLine) return;
    extract_url(firstLine,url);
    
    if(strncmp(url,"/set",4) == 0){
        get_query_params(url,"key=",key,"val=",val,NULL);
        if(key[0] && val[0]){
            if(ht_set(db,key,val,strlen(val)+1)){
                printf("200 stored\n");
            }else{
                printf("404: error in ht_set\n");
            }
        }else{
            printf("404:Bad params\n");
        }
    }else if(strncmp(url,"/get",4) == 0){
        get_query_params(url,"key=",key,NULL);
        if(key[0]){
            if((response = (char*)ht_get(db,key)) != NULL){
                printf("200: {%s}\n",response);
            }else{
                printf("404: value not found\n");
            }
        }else{
            printf("404:Bad params\n");
        }
    }else if(strncmp(url,"/delete",7) == 0){

    }else{
        printf("403: route not exists\n");
    }



}
void extract_url(char *firstLineRequest,char *dest){
    dest[0] = '\0';
    char *start = strchr(firstLineRequest,'/');
    if(!start) return;

    char *end = strchr(start,' ');
    if(!end) return;

    memcpy(dest,start,(size_t)end-start);
    dest[(size_t)end-start] = '\0';
}
void server_loop(Hash_Table* db,int server_fd){
    int newSocketFd;
    char buffer[BUFFER_SIZE] = {0};
    struct sockaddr_in clientAddress;
    int addrLen = sizeof(clientAddress);
    while(getc(stdin) != 'F') {
        // accept clients
        if ((newSocketFd = accept(server_fd, (struct sockaddr *)&clientAddress, (socklen_t*)&addrLen )) < 0) {
            perror("connection failed");
            continue;
        }

        read(newSocketFd, buffer, BUFFER_SIZE);
        printf("request received\n");
        handle_request(db,buffer);

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
