#ifndef SERVER_H
#define SERVER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "hash_table.h"

#define PORT 8080
#define BUFFER_SIZE 1024

unsigned long hash_key(const unsigned char *str,unsigned long seed);

#endif