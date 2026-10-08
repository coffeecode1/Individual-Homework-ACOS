#ifndef PARTICIPANTS_H
#define PARTICIPANTS_H

// автомобили, пешеходы и очереди из них

#include "config.h"

// Автомобиль
typedef struct Vehicle {
    int id; // номер
    int flow; // поток (направление + манёвр)
    int arrivalT; // такт прибытия
    int emergency; // 1 - экстренный транспорт
} Vehicle;

// Пешеход
typedef struct Pedestrian {
    int id; // номер
    int arrivalT; // такт прибытия
} Pedestrian;

// Полоса - очередь машин
typedef struct Flow {
    Vehicle queue[MAX_QUEUE]; // кольцевая очередь, потому что так ничего не надо сдвигать и можно просто обращаться к индексам по модулю
    int head; // индекс первой машины
    int size; // сколько машин в очереди
    int capacity; // вместимость
} Flow;

// Пешеходный переход - очередь пешеходов
typedef struct Crosswalk {
    Pedestrian queue[MAX_QUEUE];
    int head, size, capacity;
} Crosswalk;

// очередь машин
void FlowInit(Flow* f, int capacity);
int  FlowPush(Flow* f, Vehicle v); // в конец; 0 - нет места
int  FlowPushFront(Flow* f, Vehicle v); // в начало (для экстренного транспорта)
Vehicle FlowPop(Flow* f); // забрать первую
const Vehicle* FlowAt(const Flow* f, int k);// k-я машина (0 - первая) или NULL

// очередь пешеходов
void CrosswalkInit(Crosswalk* w, int capacity);
int  CrosswalkPush(Crosswalk* w, Pedestrian p);
Pedestrian CrosswalkPop(Crosswalk* w);
const Pedestrian* CrosswalkFront(const Crosswalk* w);

#endif // PARTICIPANTS_H
