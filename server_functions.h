#ifndef SERVER_FUNCTIONS_H
#define SERVER_FUNCTIONS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <stdarg.h>
#include <netinet/in.h>
#include <signal.h>
#include "hash_table.h"
#include "request_handling.h"

#define PORT 8080
#define BUFFER_SIZE 1024
#define PARAM_BUFFER_SIZE 64
#define URL_BUFFER_SIZE 1024

extern volatile sig_atomic_t keep_running;

unsigned long hash_key(const unsigned char *str,unsigned long seed);

//create and configure the server socket for listening.It returns the socket file descriptor
int start_server(int port);

//manage new clients requests
void server_loop(Hash_Table* db,int server_fd);

//well-formatting the response and send it
void send_response(int socketFd,int statusCode,char* responseMsg);

//handle Ctrl+c signal to prevent memory leak
void handle_sigint(int sig);

//configure the signals handler and flags
void config_signal_context();

#endif