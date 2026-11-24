#ifndef SIM_H
#define SIM_H

//  Все параметры перекрестка берутся из первого дз
//  Теперь каждая деталь, это отдельный поток:
//   - контроллер светофора (1 поток)
//   - стороны С/В/Ю/З (4 потока): генерируют машины и пешеходов
//   - потоки направлений - полосы (12 потоков): пропускают машину на зелёный
//   - пешеходные переходы (4 потока): пускают пешеходов на зелёный
//   - каждый автомобиль и каждый пешеход (1 поток)
//  Основные этапы такта:
//   1) потоки сторон
//   2) потоки машин, у которых истекло время проезда
//   3) решение контроллера (поток контроллера)
//   4) въезд в зону (потоки полос и переходов)
//   5) проверка инвариантов

#include <pthread.h>

#include "../crossroad.h"
#include "gate.h"

#define WORKER_COUNT (1 + DIR_COUNT + FLOW_COUNT + WALK_COUNT)

typedef enum WorkerKind {
    W_CONTROLLER, // светофор
    W_APPROACH, // подход: прибытие машин и пешеходов
    W_LANE, // полоса (поток направления)
    W_CROSSWALK // пешеходный переход
} WorkerKind;

// Где сейчас находится участник потока
typedef enum AgentState {
    A_QUEUED, // ждёт в очереди разрешающего сигнала
    A_IN_ZONE, // пересекает конфликтную зону
    A_DONE // проехал/перешёл или получил отказ, поток завершается
} AgentState;

struct Sim;

// Автомобиль или пешеход - отдельный поток
typedef struct Agent {
    pthread_t tid;
    Gate wake; // разрешение въехать в зону или покинуть её
    Gate ack; // ответ участника: действие выполнено
    int isPed; // 1 - пешеход, 0 - автомобиль
    int id; // номер участника
    int where; // полоса (машина) или переход (пешеход)
    int emergency; // 1 - экстренный транспорт
    AgentState state;
    struct Sim* sim;
    struct Agent* next; // список живых участников
} Agent;

// Служебный поток (контроллер, подход, полоса, переход)
typedef struct Worker {
    pthread_t tid;
    Gate start; // разрешение выполнить свою стадию такта
    WorkerKind kind;
    int index; // номер подхода/полосы/перехода
    unsigned seed; // свой генератор случайных чисел (rand_r)
    struct Sim* sim;
} Worker;

typedef struct Sim {
    Crossroad cr; // состояние перекрёстка из первого ДЗ
    pthread_mutex_t lock; // защищает cr, список участников и вывод событий
    Gate done; // служебные потоки сообщают об окончании стадии
    int stop; // 1 - модель останавливается, потокам пора выходить
    int zonePending; // разрешено въехать, но ещё не в зоне
    Worker workers[WORKER_COUNT];
    Agent* agents; // живые участники
    int agentsAlive; // сколько их сейчас
    int agentsMax; // максимум одновременно
    int agentsTotal; // сколько всего создано
} Sim;


// Создание участника потока и ожидание, пока он встанет в очередь
Agent* AgentSpawn(struct Sim* sim, int isPed, int where, int emergency);

void AgentWake(Agent* a);

void AgentDelete(struct Sim* sim, Agent* a);

// Поиск живого участника по номеру
Agent* AgentFind(struct Sim* sim, int id);


// Запуск/остановка служебных потоков. Возвращает 0 при успехе.
int  SimStart(struct Sim* sim, Config* cfg);
void SimStop(struct Sim* sim);

// Один такт модели (вызывает главный поток)
void SimStep(struct Sim* sim);
// Итог по потокам: на экран и в журнал
void SimThreadStats(const struct Sim* sim);

#endif
