// Служебные потоки модели (контроллер, подходы, полосы, переходы)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim.h"
#include "../view.h"

// Случайное событие с вероятностью percent процентов
static int ChanceR(unsigned* seed, int percent) {
    return rand_r(seed) % 100 < percent;
}

// Стадия "прибытие" - подход d генерирует машину и пешехода и каждый новый участник - отдельный поток, который сам встаёт в очередь.
static void ApproachStep(Worker* w) {
    Sim* sim = w->sim;
    const Config* cfg = sim->cr.cfg;
    int d = w->index;

    if(ChanceR(&w->seed, cfg->carRate[d])) {
        // случайный манёвр среди разрешённых на этом направлении
        int turns[TURN_COUNT], n = 0;
        for(int t = 0; t < TURN_COUNT; ++t)
            if(cfg->allowed[FlowIndex(d, t)]) turns[n++] = t;
        if(n > 0) {
            int flow = FlowIndex(d, turns[rand_r(&w->seed) % n]);
            int emergency = ChanceR(&w->seed, cfg->emergencyRate);
            Agent* a = AgentSpawn(sim, 0, flow, emergency);
            if(a && a->state == A_DONE) AgentDelete(sim, a); // отказ - поток уже завершился
        }
    }
    // пешеход подходит к переходу на стороне d
    if(ChanceR(&w->seed, cfg->pedRate)) {
        Agent* a = AgentSpawn(sim, 1, d, 0);
        if(a && a->state == A_DONE) AgentDelete(sim, a);
    }
}

// Стадия "въезд" - полоса i пропускает первую машину, если ей горит зелёный
static void LaneStep(Worker* w) {
    Sim* sim = w->sim;
    Crossroad* cr = &sim->cr;
    int i = w->index;

    pthread_mutex_lock(&sim->lock);
    Flow* f = &cr->flows[i];
    if(f->size == 0 || !IsGreen(cr, i) || cr->zone.count + sim->zonePending >= MAX_ZONE) {
        pthread_mutex_unlock(&sim->lock);
        return;
    }
    Vehicle v = FlowPop(f); // экстренная машина уже стоит первой
    Agent* a = AgentFind(sim, v.id);
    sim->zonePending++; // место в зоне занято за этой машиной
    pthread_mutex_unlock(&sim->lock);
    AgentWake(a); // машина сама въезжает в зону
}

// Стадия "въезд" - переход выпускает всех ожидающих пешеходов на зелёный
static void CrosswalkStep(Worker* w) {
    Sim* sim = w->sim;
    Crossroad* cr = &sim->cr;
    for(;;) {
        pthread_mutex_lock(&sim->lock);
        Crosswalk* cw = &cr->walks[w->index];
        if(cw->size == 0 || !IsPedGreen(cr) || cr->zone.count + sim->zonePending >= MAX_ZONE) {
            pthread_mutex_unlock(&sim->lock);
            return;
        }
        Pedestrian p = CrosswalkPop(cw);
        Agent* a = AgentFind(sim, p.id);
        sim->zonePending++;
        pthread_mutex_unlock(&sim->lock);
        AgentWake(a);
    }
}

// Стадия "решение контроллера" - работает как светофор из первого ДЗ
static void ControllerWorkerStep(Worker* w) {
    pthread_mutex_lock(&w->sim->lock);
    ControllerStep(&w->sim->cr);
    pthread_mutex_unlock(&w->sim->lock);
}

// Ждём свою стадию, выполняем, сообщаем о завершении
static void* WorkerThread(void* arg) {
    Worker* w = arg;
    Sim* sim = w->sim;
    for(;;) {
        GateWait(&w->start);
        if(sim->stop) break;
        switch(w->kind) {
        case W_CONTROLLER: ControllerWorkerStep(w); break;
        case W_APPROACH: ApproachStep(w); break;
        case W_LANE: LaneStep(w); break;
        case W_CROSSWALK: CrosswalkStep(w); break;
        }
        GatePost(&sim->done);
    }
    return NULL;
}

// Запуск стадии - разрешаем всем потокам вида kind работать и ждём от них сигнала о завершении
static void RunStage(Sim* sim, WorkerKind kind, WorkerKind kind2) {
    int n = 0;
    for(int k = 0; k < WORKER_COUNT; ++k)
        if(sim->workers[k].kind == kind || sim->workers[k].kind == kind2) {
            GatePost(&sim->workers[k].start);
            ++n;
        }
    while(n-- > 0) GateWait(&sim->done);
}

// Стадия "выезд" - участники, у которых закончилось время проезда покидают зону
static void LeaveStage(Sim* sim) {
    Agent* due[MAX_ZONE];
    int n = 0;
    pthread_mutex_lock(&sim->lock);
    for(int k = 0; k < sim->cr.zone.count; ++k) {
        const Occupant* o = &sim->cr.zone.items[k];
        if(o->leaveT > sim->cr.t) continue; // ещё едет или идет
        Agent* a = AgentFind(sim, o->id);
        if(a) due[n++] = a;
    }
    pthread_mutex_unlock(&sim->lock);
    for(int k = 0; k < n; ++k) {
        AgentWake(due[k]); // участник сам выходит из зоны
        AgentDelete(sim, due[k]); // и его поток завершается
    }
}

// Проверка инвариантов
static void CheckInvariantsMT(Sim* sim) {
    Crossroad* cr = &sim->cr;
    const Config* cfg = cr->cfg;
    const Zone* z = &cr->zone;
    char msg[160];

    // Конфликтующие направления не получают зелёный одновременно
    for(int i = 0; i < FLOW_COUNT; ++i)
        for(int j = i + 1; j < FLOW_COUNT; ++j)
            if(IsGreen(cr, i) && IsGreen(cr, j) && cfg->conflict[i][j]) {
                snprintf(msg, sizeof(msg), "зелёный одновременно для %s и %s",
                         FlowName(i), FlowName(j));
                InvariantViolation(cr, msg);
            }
    // В зоне нет конфликтующих участников
    for(int a = 0; a < z->count; ++a)
        for(int b = a + 1; b < z->count; ++b) {
            const Occupant* x = &z->items[a];
            const Occupant* y = &z->items[b];
            if(x->isPed != y->isPed) {
                InvariantViolation(cr, "в зоне одновременно машина и пешеход");
                a = z->count;
                break;
            }
            if(!x->isPed && cfg->conflict[x->where][y->where]) {
                snprintf(msg, sizeof(msg), "в зоне конфликтующие машины #%d и #%d", x->id, y->id);
                InvariantViolation(cr, msg);
            }
        }
    // Очереди не переполнены, машина находится только в одном месте
    static int ids[FLOW_COUNT * MAX_QUEUE + MAX_ZONE];
    int n = 0;
    for(int i = 0; i < FLOW_COUNT; ++i) {
        if(cr->flows[i].size > cr->flows[i].capacity)
            InvariantViolation(cr, "очередь переполнена");
        for(int k = 0; k < cr->flows[i].size; ++k)
            ids[n++] = FlowAt(&cr->flows[i], k)->id;
    }
    for(int k = 0; k < z->count; ++k)
        if(!z->items[k].isPed) ids[n++] = z->items[k].id;
    for(int a = 0; a < n; ++a)
        for(int b = a + 1; b < n; ++b)
            if(ids[a] == ids[b]) {
                snprintf(msg, sizeof(msg), "автомобиль #%d находится в двух местах", ids[a]);
                InvariantViolation(cr, msg);
            }
}

void SimStep(Sim* sim) {
    RunStage(sim, W_APPROACH, W_APPROACH); // 1) прибытие
    LeaveStage(sim); // 2) выезд из зоны
    RunStage(sim, W_CONTROLLER, W_CONTROLLER); // 3) светофор
    RunStage(sim, W_LANE, W_CROSSWALK); // 4) въезд
    pthread_mutex_lock(&sim->lock);
    CheckInvariantsMT(sim); // 5) инварианты
    pthread_mutex_unlock(&sim->lock);
}

int SimStart(Sim* sim, Config* cfg) {
    memset(sim, 0, sizeof(*sim));
    pthread_mutex_init(&sim->lock, NULL);
    if(GateInit(&sim->done, 0) != 0) {
        dprintf(2, "Ошибка: не удалось создать семафор\n");
        return -1;
    }

    pthread_mutex_lock(&sim->lock);
    CrossroadInit(&sim->cr, cfg); // включает первую фазу и выводит событие
    pthread_mutex_unlock(&sim->lock);

    // роль каждого потока
    int k = 0;
    sim->workers[k++] = (Worker){ .kind = W_CONTROLLER, .index = 0 };
    for(int d = 0; d < DIR_COUNT; ++d)  sim->workers[k++] = (Worker){ .kind = W_APPROACH, .index = d };
    for(int i = 0; i < FLOW_COUNT; ++i) sim->workers[k++] = (Worker){ .kind = W_LANE, .index = i };
    for(int w = 0; w < WALK_COUNT; ++w) sim->workers[k++] = (Worker){ .kind = W_CROSSWALK, .index = w };
    for(k = 0; k < WORKER_COUNT; ++k) {
        Worker* w = &sim->workers[k];
        w->sim = sim;
        w->seed = cfg->seed * 31u + (unsigned)k + 1u; // воспроизводимо при одном seed
        if(GateInit(&w->start, 0) != 0 || pthread_create(&w->tid, NULL, WorkerThread, w) != 0) {
            dprintf(2, "Ошибка: не удалось создать служебный поток\n");
            return -1;
        }
    }
    return 0;
}

void SimStop(Sim* sim) {
    sim->stop = 1;
    // будим всех участников которые ещё ждут в очередях и в зоне
    for(;;) {
        pthread_mutex_lock(&sim->lock);
        Agent* a = sim->agents;
        pthread_mutex_unlock(&sim->lock);
        if(a == NULL) break;
        AgentWake(a);
        AgentDelete(sim, a);
    }
    // если потоки сейчас ждут своей стадии - отпускаем их
    for(int k = 0; k < WORKER_COUNT; ++k) GatePost(&sim->workers[k].start);
    for(int k = 0; k < WORKER_COUNT; ++k) {
        pthread_join(sim->workers[k].tid, NULL);
        GateDestroy(&sim->workers[k].start);
    }
    GateDestroy(&sim->done);
    pthread_mutex_destroy(&sim->lock);
}

// Итог по потокам в дескриптор fd
static void ThreadStatsTo(int fd, const Sim* sim) {
    dprintf(fd, "Потоки: служебных %d (контроллер, 4 подхода, 12 полос, 4 перехода), "
                "участников создано %d, одновременно максимум %d\n",
            WORKER_COUNT, sim->agentsTotal, sim->agentsMax);
    dprintf(fd, "Синхронизация: %s\n", GateKind());
}

void SimThreadStats(const Sim* sim) {
    ThreadStatsTo(1, sim);
    if(ViewLogFd() != -1) ThreadStatsTo(ViewLogFd(), sim);
}
