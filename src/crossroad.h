#ifndef CROSSROAD_H
#define CROSSROAD_H
//==============================================================================
//  crossroad.h
//==============================================================================
//  Модель перекрёстка целиком: очереди полос, очереди у переходов,
//  конфликтная зона, контроллер и статистика.
//  За один такт выполняются:
//    1) прибытие новых участников;
//    2) выезд из зоны тех, кто закончил проезд/переход;
//    3) решение контроллера;
//    4) въезд в зону участников с разрешающим сигналом;
//    5) проверка инвариантов.
//==============================================================================

#include "config.h"
#include "participants.h"
#include "controller.h"

#define MAX_ZONE 128 // максимум участников одновременно в конфликтной зоне

// Участник внутри конфликтной зоны
typedef struct Occupant {
    int isPed; // 1 - пешеход, 0 - автомобиль
    int id; // номер участника
    int where; // поток (машина) или переход (пешеход)
    int leaveT; // такт, на котором он покинет зону
} Occupant;

// Конфликтная зона перекрёстка
typedef struct Zone {
    Occupant items[MAX_ZONE];
    int count;
} Zone;

// Статистика (задержка = сколько тактов участник ждал разрешения)
typedef struct Stats {
    int  carsArrived, carsPassed, carsRefused;
    long carDelay;      int carMaxDelay; // сумма и максимум задержек машин
    int  pedsArrived, pedsPassed, pedsRefused;
    long pedDelay;      int pedMaxDelay;
    int  emergencyArrived, emergencyPassed;
    long emergencyDelay;
    int  phaseSwitches; // сколько раз включалась фаза
    int  violations; // нарушений инвариантов
} Stats;

typedef struct Crossroad {
    Config*    cfg; // параметры
    int        t; // текущий такт
    Flow       flows[FLOW_COUNT]; // очереди машин по полосам
    Crosswalk  walks[WALK_COUNT]; // очереди пешеходов у переходов
    Zone       zone; // конфликтная зона
    Controller ctrl; // светофор
    Stats      stats; // статистика
    int        nextId; // счётчик номеров участников
} Crossroad;

// Создание начального состояния перекрёстка
void CrossroadInit(Crossroad* cr, Config* cfg);

// Один такт имитации
void CrossroadStep(Crossroad* cr);

// Стадии такта для последовательной и многопроцессной версий.
void CarArrivalStep(Crossroad* cr);
void PedArrivalStep(Crossroad* cr);
void LeaveStep(Crossroad* cr, int isPed); // -1: все, 0: авто, 1: пешеходы
void EnterDirection(Crossroad* cr, int dir);
void CheckInvariants(Crossroad* cr);

// Сколько участников пропущено на перекрёсток
static inline int CrossroadPassed(const Crossroad* cr) {
    return cr->stats.carsPassed + cr->stats.pedsPassed;
}

// Регистрация нарушения инварианта
void InvariantViolation(Crossroad* cr, const char* what);

#endif // CROSSROAD_H
