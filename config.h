#ifndef CONFIG_H
#define CONFIG_H

#define PORT             8080
#define BUFFER_SIZE      (1<<10)
#define URL_BUFFER_SIZE  (1<<10) 
#define PARAM_KEY_SIZE   (1<<6) 
#define PARAM_VALUE_SIZE (1<<10) 
#define RESPONSE_BUFFER_SIZE (PARAM_VALUE_SIZE + 256)
#define LISTEN_BACKLOG 3

#endif