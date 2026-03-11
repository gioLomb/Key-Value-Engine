
#include <stdio.h>
typedef struct Task {
    int socketFd;
    struct Task *next;
} Task;
int main(){printf("%zu\n",sizeof(Task));}