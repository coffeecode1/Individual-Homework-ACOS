#ifndef PROCESS_IPC_H
#define PROCESS_IPC_H

#include <errno.h>
#include <semaphore.h>
#include <signal.h>
#include <stddef.h>
#include <sys/types.h>

#include "../crossroad.h"

// Порядок процессов направлений совпадает с порядком DIR_N/DIR_W.
enum Role { CAR, PED, LIGHT, NORTH, EAST, SOUTH, WEST, ROLE_COUNT };
enum { READY = ROLE_COUNT, DONE, STOP, CLAIM, SEM_COUNT = CLAIM + ROLE_COUNT };
enum Channel { REQUEST, RESPONSE };
enum { READ_END, WRITE_END };
enum Mode { FORK_NAMED, FORK_UNNAMED, INDEPENDENT_SHM, INDEPENDENT_FIFO };

typedef struct Packet {
    Config cfg;
    Crossroad cr; // cfg внутри cr равен NULL при передаче
    int leave; // задание для машин/пешеходов: 0 - прибытие, 1 - выезд
    size_t eventBytes;
    char events[512 * (MAX_ZONE + 1)];
} Packet;

typedef struct Shared {
    sem_t sem[SEM_COUNT]; // используется только в режиме FORK_UNNAMED
    Packet packet;
} Shared;

typedef struct Ipc {
    enum Mode mode;
    const char* session;
    pid_t owner;
    Shared* shared;
    sem_t* sem[SEM_COUNT];
    int semCount;
    int dir; // каталог для независимого запуска
    int fd[2][2]; // канал и его конец
} Ipc;

extern const char* const RoleNames[ROLE_COUNT];

int IpcCreate(Ipc* ipc, enum Mode mode, const char* session);
int IpcPublish(const Ipc* ipc);
int IpcConnect(Ipc* ipc, const char* session, enum Role role);
void IpcClose(Ipc* ipc); // вызывается координатором

int IpcWait(sem_t* sem, int seconds, const volatile sig_atomic_t* interrupted);
int FifoTransfer(const Ipc* ipc, enum Channel channel, Packet* packet, int writing);

int WorkerRun(Ipc* ipc, enum Role role);

static inline int IpcError(int error) {
    errno = error;
    return -1;
}

#endif
