#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

int NetOpen(const char* ip, const char* port, int server) {
    char* end;
    long number = strtol(port, &end, 10);
    struct sockaddr_in address = { .sin_family = AF_INET };
    if(!*port || *end || number < 1 || number > 65535 ||
       inet_pton(AF_INET, ip, &address.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    address.sin_port = htons((unsigned short)number);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd == -1) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval timeout = { .tv_sec = 65 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    int rc = server ? bind(fd, (struct sockaddr*)&address, sizeof(address))
        : connect(fd, (struct sockaddr*)&address, sizeof(address));
    if(rc == 0 && server) rc = listen(fd, 64);
    if(rc == 0) return fd;
    int error = errno;
    close(fd);
    errno = error;
    return -1;
}

// send может принять только часть строки
int NetSend(int fd, const char* text) {
    size_t left = strlen(text);
    while(left) {
        ssize_t count = send(fd, text, left, 0);
        if(count == -1 && errno == EINTR) continue;
        if(count <= 0) return -1;
        text += count;
        left -= (size_t)count;
    }
    return 0;
}

int NetLine(int fd, char* line, size_t size) {
    // читаем до переноса даже если сообщение пришло не одним куском
    for(size_t i = 0; i + 1 < size; ++i) {
        ssize_t count;
        do { count = recv(fd, line + i, 1, 0); } while(count == -1 && errno == EINTR);
        if(count <= 0) return (int)count;
        if(line[i] == '\n') {
            line[i] = '\0';
            return 1;
        }
    }
    errno = EMSGSIZE;
    return -1;
}

long long NetTime(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
