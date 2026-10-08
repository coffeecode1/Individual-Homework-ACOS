//  Очереди машин и пешеходов

#include <string.h>
#include "participants.h"

void FlowInit(Flow* f, int capacity) {
    memset(f, 0, sizeof(*f));
    f->capacity = capacity;
}

// Постановка машины в конец очереди. Возвращает 0, если очередь полна
int FlowPush(Flow* f, Vehicle v) {
    if(f->size >= f->capacity) return 0;
    f->queue[(f->head + f->size) % MAX_QUEUE] = v;
    f->size++;
    return 1;
}

// Постановка машины в начало очереди (остальные её пропускают)
int FlowPushFront(Flow* f, Vehicle v) {
    if(f->size >= f->capacity) return 0;
    f->head = (f->head - 1 + MAX_QUEUE) % MAX_QUEUE;
    f->queue[f->head] = v;
    f->size++;
    return 1;
}

// Извлечение первой машины (вызывать только если очередь не пуста)
Vehicle FlowPop(Flow* f) {
    Vehicle v = f->queue[f->head];
    f->head = (f->head + 1) % MAX_QUEUE;
    f->size--;
    return v;
}

// k-я машина очереди (0 - первая) или NULL
const Vehicle* FlowAt(const Flow* f, int k) {
    return (k >= 0 && k < f->size) ? &f->queue[(f->head + k) % MAX_QUEUE] : NULL;
}

//------------------------------------------------------------------------------
void CrosswalkInit(Crosswalk* w, int capacity) {
    memset(w, 0, sizeof(*w));
    w->capacity = capacity;
}

int CrosswalkPush(Crosswalk* w, Pedestrian p) {
    if(w->size >= w->capacity) return 0;
    w->queue[(w->head + w->size) % MAX_QUEUE] = p;
    w->size++;
    return 1;
}

Pedestrian CrosswalkPop(Crosswalk* w) {
    Pedestrian p = w->queue[w->head];
    w->head = (w->head + 1) % MAX_QUEUE;
    w->size--;
    return p;
}

const Pedestrian* CrosswalkFront(const Crosswalk* w) {
    return w->size > 0 ? &w->queue[w->head] : NULL;
}
