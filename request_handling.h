#ifndef REQUEST_HANDLING_H
#define REQUEST_HANDLING_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <stdarg.h>
#include <netinet/in.h>
#include "hash_table.h"
#include "server_functions.h"

//parse the requests and verify the url route filling the response buffer with a specific message.
//It returns the status code
int handle_request(Hash_Table* db,char* requestBuffer,char *responseBuffer);

//extract the url from the request body; it fills the dest buffer with url
void extract_url(char *firstLineRequest,char *dest);

//extract the query params. It checks the params name and write their value in the specified buffer
void get_query_params(const char *url,...);

#endif