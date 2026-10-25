//  Потоки участники - автомобили и пешеход
//  Каждый участник проходит путь: прибытие -> очередь -> (разрешение) -> конфликтная зона -> (время проезда истекло) -> выход

#include <stdio.h>
#include <stdlib.h>

#include "sim.h"
#include "../view.h"

// Названия переходов из crossroad.c
extern const char* WALK_NAMES[WALK_COUNT];
extern const char* WALK_TO[WALK_COUNT];

Agent* AgentFind(Sim* sim, int id) {
    for(Agent* a = sim->agents; a != NULL; a = a->next)
        if(a->id == id) return a;
    return NULL;
}

// Конец шага участника - отпускаем модель и сообщаем, что шаг выполнен
static void StepDone(Agent* a) {
    pthread_mutex_unlock(&a->sim->lock);
    GatePost(&a->ack);
}

// Ожидание разрешения на следующий шаг. Возвращает 0, если модель останавливается и потоку пора завершаться.
static int WaitTurn(Agent* a) {
    GateWait(&a->wake);
    if(!a->sim->stop) return 1;
    GatePost(&a->ack);
    return 0;
}

// Последний шаг - участник покидает конфликтную зону
static void LeaveZone(Agent* a) {
    Crossroad* cr = &a->sim->cr;
    pthread_mutex_lock(&a->sim->lock);
    Zone* z = &cr->zone;
    int k = 0;
    while(k < z->count && z->items[k].id != a->id) ++k;
    if(k < z->count) z->items[k] = z->items[--z->count]; // на его место ставим последнего
    else InvariantViolation(cr, "участник покидает зону, в которой его нет");
    a->state = A_DONE;
    if(a->isPed)
        EventOut(NULL, "Пешеход #%d завершил переход (%s переход)", a->id, WALK_NAMES[a->where]);
    else
        EventOut(NULL, "Авто #%d проехало перекрёсток (%s)", a->id, FlowName(a->where));
    StepDone(a);
}

// Поток автомобиля
static void* VehicleThread(void* arg) {
    Agent* a = arg;
    Crossroad* cr = &a->sim->cr;
    const Config* cfg = cr->cfg;
    Stats* st = &cr->stats;
    int d = FlowDir(a->where);
    const char* who = a->emergency ? "ЭКСТРЕННОЕ авто" : "Авто";

    // 1) прибытие: встаём в очередь своей полосы
    pthread_mutex_lock(&a->sim->lock);
    Vehicle v = { a->id, a->where, cr->t, a->emergency };
    Flow* f = &cr->flows[a->where];
    st->carsArrived++;
    if(v.emergency) st->emergencyArrived++;
    if(f->size >= f->capacity) {
        // переполнение очереди - затор, участник уезжает
        st->carsRefused++;
        EventOut(C_RED, "%s #%d прибыло %s (%s): очередь %s заполнена - ОТКАЗ, затор",
                 who, v.id, DIR_FROM[d], TURN_NAMES[FlowTurn(v.flow)], FlowName(v.flow));
        a->state = A_DONE;
        StepDone(a);
        return NULL;
    }
    int front = v.emergency && cfg->emergencyPriority;
    if(front) FlowPushFront(f, v);
    else      FlowPush(f, v);
    a->state = A_QUEUED;
    EventOut(v.emergency ? C_PURPLE : NULL, "%s #%d прибыло %s (%s) - %s очереди %s: %d/%d",
             who, v.id, DIR_FROM[d], TURN_NAMES[FlowTurn(v.flow)],
             front ? "В НАЧАЛО" : "в конец", FlowName(v.flow), f->size, f->capacity);
    StepDone(a);

    // 2) ждём, пока поток полосы снимет нас с очереди на зелёный и въезжаем
    if(!WaitTurn(a)) return NULL;
    pthread_mutex_lock(&a->sim->lock);
    // инвариант: въезд в зону только при разрешающем сигнале
    if(!IsGreen(cr, a->where))
        InvariantViolation(cr, "автомобиль въехал в зону без разрешающего сигнала");
    int delay = cr->t - v.arrivalT;
    st->carsPassed++;
    st->carDelay += delay;
    if(delay > st->carMaxDelay) st->carMaxDelay = delay;
    if(v.emergency) {
        st->emergencyPassed++;
        st->emergencyDelay += delay;
    }
    cr->zone.items[cr->zone.count++] = (Occupant){0, v.id, a->where, cr->t + cfg->crossTime};
    a->sim->zonePending--;
    a->state = A_IN_ZONE;
    EventOut(v.emergency ? C_PURPLE : C_GREEN,
             "%s #%d: разрешено движение, въезжает (%s), ждало %d такт.; очередь %s: %d/%d",
             who, v.id, FlowName(a->where), delay, FlowName(a->where), f->size, f->capacity);
    StepDone(a);

    // 3) ждём окончания проезда и покидаем зону
    if(WaitTurn(a)) LeaveZone(a);
    return NULL;
}

// Поток пешехода
static void* PedestrianThread(void* arg) {
    Agent* a = arg;
    Crossroad* cr = &a->sim->cr;
    Stats* st = &cr->stats;
    int w = a->where;

    // 1) подходим к переходу, при необходимости нажимаем кнопку вызова
    pthread_mutex_lock(&a->sim->lock);
    Pedestrian p = { a->id, cr->t };
    Crosswalk* cw = &cr->walks[w];
    st->pedsArrived++;
    if(cw->size >= cw->capacity) {
        st->pedsRefused++;
        EventOut(C_RED, "Пешеход #%d подошёл к %s переходу: мест нет - ОТКАЗ", p.id, WALK_TO[w]);
        a->state = A_DONE;
        StepDone(a);
        return NULL;
    }
    CrosswalkPush(cw, p);
    a->state = A_QUEUED;
    if(!IsPedGreen(cr) && !cr->ctrl.pedRequest) {
        cr->ctrl.pedRequest = 1;
        EventOut(C_CYAN, "Пешеход #%d подошёл к %s переходу и НАЖАЛ КНОПКУ вызова",
                 p.id, WALK_TO[w]);
    } else {
        EventOut(NULL, "Пешеход #%d подошёл к %s переходу и ждёт (ждут: %d)",
                 p.id, WALK_TO[w], cw->size);
    }
    StepDone(a);

    // 2) ждём зелёного для пешеходов и начинаем переход
    if(!WaitTurn(a)) return NULL;
    pthread_mutex_lock(&a->sim->lock);
    if(!IsPedGreen(cr))
        InvariantViolation(cr, "пешеход вышел на переход без разрешающего сигнала");
    int delay = cr->t - p.arrivalT;
    st->pedsPassed++;
    st->pedDelay += delay;
    if(delay > st->pedMaxDelay) st->pedMaxDelay = delay;
    cr->zone.items[cr->zone.count++] = (Occupant){1, p.id, w, cr->t + cr->cfg->crossTime};
    a->sim->zonePending--;
    a->state = A_IN_ZONE;
    EventOut(C_GREEN, "Пешеход #%d: зелёный, начал переход (%s переход), ждал %d такт.",
             p.id, WALK_NAMES[w], delay);
    StepDone(a);

    // 3) заканчиваем переход
    if(WaitTurn(a)) LeaveZone(a);
    return NULL;
}

Agent* AgentSpawn(Sim* sim, int isPed, int where, int emergency) {
    Agent* a = calloc(1, sizeof(*a));
    if(a == NULL) return NULL;
    GateInit(&a->wake, 0);
    GateInit(&a->ack, 0);
    a->isPed = isPed;
    a->where = where;
    a->emergency = emergency;
    a->sim = sim;

    // номер выдаём и добавляем участника в список до запуска его потока
    pthread_mutex_lock(&sim->lock);
    a->id = ++sim->cr.nextId;
    a->next = sim->agents;
    sim->agents = a;
    sim->agentsTotal++;
    if(++sim->agentsAlive > sim->agentsMax) sim->agentsMax = sim->agentsAlive;
    pthread_mutex_unlock(&sim->lock);

    if(pthread_create(&a->tid, NULL, isPed ? PedestrianThread : VehicleThread, a) != 0) {
        dprintf(2, "Ошибка: не удалось создать поток участника #%d\n", a->id);
        exit(1);
    }
    GateWait(&a->ack); // ждём, пока участник встанет в очередь
    return a;
}

void AgentWake(Agent* a) {
    GatePost(&a->wake);
    GateWait(&a->ack);
}

// Память освобождает тот, кто ждал участника, и только после pthread_join:
// так поток участника гарантированно больше не обращается к своим семафорам
void AgentDelete(Sim* sim, Agent* a) {
    pthread_join(a->tid, NULL);
    pthread_mutex_lock(&sim->lock);
    Agent** p = &sim->agents;
    while(*p != a) p = &(*p)->next;
    *p = a->next; // убираем из списка живых участников
    sim->agentsAlive--;
    pthread_mutex_unlock(&sim->lock);
    GateDestroy(&a->wake);
    GateDestroy(&a->ack);
    free(a);
}
