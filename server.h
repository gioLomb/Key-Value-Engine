#ifndef SERVER_H
#define SERVER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <stdarg.h>
#include <netinet/in.h>
#include "hash_table.h"

#define PORT 8080
#define BUFFER_SIZE 1024

unsigned long hash_key(const unsigned char *str,unsigned long seed);
int start_server(int port);
void server_loop(Hash_Table* db,int server_fd);
void handle_request(Hash_Table* db,char* requestBuffer,char *responseBuffer);
void extract_url(char *firstLineRequest,char *dest);
void get_query_params(const char *url,...);

#endif