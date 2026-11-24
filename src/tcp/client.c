#include "net.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
#ifdef TCP_OBSERVER
    if(argc != 3) {
        dprintf(2, "Запуск: %s IP ПОРТ\n", argv[0]);
        return 1;
    }
    const char* hello = "WATCH\n";
#else
    const char* names[] = {"north", "east", "south", "west"};
    int dir = 0;
    if(argc == 4)
        while(dir < 4 && strcmp(argv[3], names[dir])) ++dir;
    if(argc != 4 || dir == 4) {
        dprintf(2, "Запуск: %s IP ПОРТ north|east|south|west\n", argv[0]);
        return 1;
    }
    char hello[32];
    snprintf(hello, sizeof(hello), "WORK %d\n", dir);
    int carRate = 0, pedRate = 0, emergencyRate = 0, allowed = 0;
    unsigned seed = 0;
#endif
    signal(SIGPIPE, SIG_IGN);
    int fd = NetOpen(argv[1], argv[2], 0);
    if(fd == -1) { perror("Подключение"); return 1; }
    int failed = NetSend(fd, hello) == -1;
    char line[NET_LINE];
    int received = 0;
    while(!failed && (received = NetLine(fd, line, sizeof(line))) > 0) {
        if(strncmp(line, "ERROR ", 6) == 0) {
            dprintf(2, "%s\n", line);
            close(fd);
            return 1;
        }
#ifndef TCP_OBSERVER
        int tick;
        if(sscanf(line, "CONFIG %d %d %d %d %u", &carRate, &pedRate,
                  &emergencyRate, &allowed, &seed) == 5) {
            continue;
        }
        if(sscanf(line, "TICK %d", &tick) == 1) {
            // один и тот же такт после переподключения даст те же прибытия
            srand(seed + (unsigned)tick * 31u);
            int turn = -1, emergency = 0;
            int turns[3], count = 0;
            for(int i = 0; i < 3; ++i)
                if(allowed & (1 << i)) turns[count++] = i;
            if(rand() % 100 < carRate && count) {
                turn = turns[rand() % count];
                emergency = rand() % 100 < emergencyRate;
            }
            int ped = rand() % 100 < pedRate;
            char reply[80];
            snprintf(reply, sizeof(reply), "ARRIVE %d %d %d %d\n", tick, turn, emergency, ped);
            failed = NetSend(fd, reply) == -1;
            dprintf(1, "[t=%d] %s: манёвр %d (-1 нет авто), экстренное %d, пешеход %d\n",
                    tick, argv[3], turn, emergency, ped);
            continue;
        }
#endif
        dprintf(1, "%s\n", line);
        if(strncmp(line, "END ", 4) == 0) {
            close(fd);
            return failed;
        }
    }
    if(received == -1 || failed) perror("Обмен с сервером");
    else dprintf(2, "Сервер закрыл соединение\n");
    close(fd);
    return 1;
}
