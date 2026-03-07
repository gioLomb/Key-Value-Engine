#ifndef CONFIG_H
#define CONFIG_H
#include <pthread.h>

#define PORT             8080
#define BUFFER_SIZE      (1<<10)
#define URL_BUFFER_SIZE  (1<<10)
#define PARAM_KEY_SIZE   (1<<8)
#define PARAM_VALUE_SIZE BUFFER_SIZE

#endif