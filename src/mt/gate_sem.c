#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "gate.h"

int GateInit(Gate* g, unsigned value) {
#ifdef __APPLE__
    static unsigned counter = 0;
    char name[32];
    snprintf(name, sizeof(name), "/cr-%d-%u", (int)getpid(),
             __atomic_fetch_add(&counter, 1, __ATOMIC_RELAXED));
    g->sem = sem_open(name, O_CREAT | O_EXCL, 0600, value);
    if(g->sem == SEM_FAILED) return -1;
    sem_unlink(name);
    return 0;
#else
    g->sem = &g->storage;
    return sem_init(g->sem, 0, value);
#endif
}

void GateDestroy(Gate* g) {
#ifdef __APPLE__
    sem_close(g->sem);
#else
    sem_destroy(g->sem);
#endif
}

void GatePost(Gate* g) {
    sem_post(g->sem);
}

// sem_wait может прерваться сигналом (Ctrl+C) - тогда просто ждём дальше,
// а флаг остановки главный поток проверит в конце такта.
void GateWait(Gate* g) {
    while(sem_wait(g->sem) == -1 && errno == EINTR) {}
}

const char* GateKind(void) {
    return "семафоры + мьютекс";
}
