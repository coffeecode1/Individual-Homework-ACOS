#ifndef TCP_NET_H
#define TCP_NET_H

#include <stddef.h>

#define NET_LINE 1024

// В TCP границы сообщений не сохраняются, поэтому каждое заканчиваем переносом
int NetOpen(const char* ip, const char* port, int server);
int NetSend(int fd, const char* text);
int NetLine(int fd, char* line, size_t size);
long long NetTime(void);

#endif
