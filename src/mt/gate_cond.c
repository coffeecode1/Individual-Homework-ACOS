//  Gate на мьютексе и условной переменной (вместо семафоров).

#include "gate.h"

int GateInit(Gate* g, unsigned value) {
    g->count = value;
    if(pthread_mutex_init(&g->lock, NULL) != 0) return -1;
    if(pthread_cond_init(&g->cond, NULL) != 0) {
        pthread_mutex_destroy(&g->lock);
        return -1;
    }
    return 0;
}

void GateDestroy(Gate* g) {
    pthread_cond_destroy(&g->cond);
    pthread_mutex_destroy(&g->lock);
}

void GatePost(Gate* g) {
    pthread_mutex_lock(&g->lock);
    g->count++;
    pthread_cond_signal(&g->cond); // будим одного ожидающего
    pthread_mutex_unlock(&g->lock);
}

void GateWait(Gate* g) {
    pthread_mutex_lock(&g->lock);
    while(g->count == 0) // цикл - защита от ложных пробуждений
        pthread_cond_wait(&g->cond, &g->lock);
    g->count--;
    pthread_mutex_unlock(&g->lock);
}

const char* GateKind(void) {
    return "условные переменные + мьютекс";
}
