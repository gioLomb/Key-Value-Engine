#ifndef ROUTE_HANDLER_H
#define ROUTE_HANDLER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <stdarg.h>
#include <netinet/in.h>
#include "hash_table.h"
#include "config.h"


typedef struct Route{
    char *path;
    int (*handler)(Hash_Table* table, const char* url, char* responseBuffer);
} Route;

//parse the requests and verify the url route filling the response buffer with a specific message.
//It returns the status code.
int handle_request(Hash_Table* db,char* requestBuffer,char *responseBuffer);

#endif