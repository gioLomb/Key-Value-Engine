#include "server_functions.h"
volatile sig_atomic_t keep_running = 1;

int main(int argc,char **argv){
    config_signal_context();
    Hash_Table *db = ht_create(5,hash_key);
    if(ht_load(db,argv[1])){
        printf("Table loaded from %s\n",argv[1]);
    }else{
        printf("Empty table\n");
    }

    server_loop(db,start_server(PORT));

    printf("EXIT");
    ht_destroy(db,argv[1]);
}

void config_signal_context(){
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
}

void handle_sigint(int sig) {
    printf("ctrl+c\n");
    keep_running = 0;
}

//handle the client context for request parsing and response sending
static void *handle_client(void *arg) {
    ClientContext *ctx = (ClientContext*)arg;
    char requestBuffer[BUFFER_SIZE]  = {0};
    char responseBuffer[BUFFER_SIZE] = {0};

    ssize_t nBytes = read(ctx->socketFd, requestBuffer, BUFFER_SIZE - 1);
    if(nBytes > 0) requestBuffer[nBytes] = '\0';

    int statusCode = handle_request(ctx->db, requestBuffer, responseBuffer, ctx->rwlock);
    send_response(ctx->socketFd, statusCode, responseBuffer);
    close(ctx->socketFd);
    free(ctx);
    return NULL; //return value not needed
}

void server_loop(Hash_Table* db,int server_fd){
    int newSocketFd;
    struct sockaddr_in clientAddress;
    int addrLen = sizeof(clientAddress);

    while(keep_running) {
        // accept clients
        newSocketFd = accept(server_fd, (struct sockaddr *)&clientAddress, (socklen_t*)&addrLen );
        if (newSocketFd < 0) {
            //exit from loop when Ctrl+c
            if(keep_running==0) break;
            perror("connection failed");
            continue;
        }

        //new client
        ClientContext *ctx = malloc(sizeof(ClientContext));
        if(!ctx) { close(newSocketFd); continue; }
        ctx->socketFd = newSocketFd;
        ctx->db = db;

        //new thread for client
        pthread_t thread;
        if(pthread_create(&thread, NULL, handle_client, ctx) != 0) {
            perror("pthread_create failed");
            free(ctx);
            close(newSocketFd);
            continue;
        }
        pthread_detach(thread);
    }
}

void send_response(int socketFd,int statusCode,char* responseMsg){
    char fullResponseBuffer[BUFFER_SIZE+512];
    char *statusMsg;

    switch(statusCode){
        case 200:
            statusMsg="OK"; break;
        case 400:
            statusMsg="Bad Request"; break;
        case 404:
            statusMsg="Not Found"; break;
        default:
            statusMsg="Internal Server Error"; break;
    }
    int len = snprintf(fullResponseBuffer, sizeof(fullResponseBuffer),
        "HTTP/1.1 %d %s\r\n" 
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s", 
        statusCode, statusMsg, strlen(responseMsg), responseMsg);

        write(socketFd,fullResponseBuffer,len);
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
