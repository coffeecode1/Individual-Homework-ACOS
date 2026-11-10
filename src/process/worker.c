#include "ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../view.h"

static void SaveEvent(const char* msg, void* data) {
    Packet* packet = data;
    size_t size = strlen(msg) + 1;
    // Собираем события, потом их печатаем
    if(size > sizeof(packet->events) - packet->eventBytes) return;
    memcpy(packet->events + packet->eventBytes, msg, size);
    packet->eventBytes += size;
}

int WorkerRun(Ipc* ipc, enum Role role) {
    signal(SIGINT, SIG_IGN); // обрабатываем Ctrl+C
    signal(SIGTERM, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);
    Packet local;
    Packet* packet = ipc->shared ? &ipc->shared->packet : &local;
    if(sem_post(ipc->sem[READY]) == -1) return -1;

    int first = 1;
    for(;;) {
        // Ждем пока не позовут нашу роль
        if(IpcWait(ipc->sem[role], 1, NULL) == -1) {
            if(errno == ETIMEDOUT && kill(ipc->owner, 0) == 0) continue;
            return -1;
        }
        if(sem_trywait(ipc->sem[STOP]) == 0) return sem_post(ipc->sem[READY]);
        if(errno != EAGAIN) return -1;
        if(!ipc->shared && FifoTransfer(ipc, REQUEST, packet, 0) == -1) return -1;

        if(first) srand(packet->cfg.seed + (unsigned)role);
        first = 0;

        // Каждый процесс подставляет свой адрес конфига
        packet->cr.cfg = &packet->cfg;
        packet->eventBytes = 0;
        ViewSetEventSink(SaveEvent, packet);
        if(packet->leave) LeaveStep(&packet->cr, role == PED);
        else if(role == CAR) CarArrivalStep(&packet->cr);
        else if(role == PED) PedArrivalStep(&packet->cr);
        else if(role == LIGHT) ControllerStep(&packet->cr);
        else EnterDirection(&packet->cr, role - NORTH);
        ViewSetEventSink(NULL, NULL);
        packet->cr.cfg = NULL;

        int sent = ipc->shared ? sem_post(ipc->sem[DONE])
                               : FifoTransfer(ipc, RESPONSE, packet, 1);
        if(sent == -1) return -1;
    }
}

#ifdef PROCESS_WORKER
int main(int argc, char** argv) {
    int role = 0;
    if(argc == 3)
        while(role < ROLE_COUNT && strcmp(argv[2], RoleNames[role]) != 0) ++role;
    if(argc != 3 || role == ROLE_COUNT) {
        dprintf(2, "Запуск: %s КАТАЛОГ cars|pedestrians|controller|north|east|south|west\n",
                argv[0]);
        return 1;
    }

    Ipc ipc;
    if(IpcConnect(&ipc, argv[1], role) == 0 && WorkerRun(&ipc, role) == 0) return 0;
    perror(RoleNames[role]);
    return 1;
}
#endif
