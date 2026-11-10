//  Пошаговая имитация перекрёстка: прибытие участников, их въезд в
//  конфликтную зону и выезд из неё, проверка инвариантов.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crossroad.h"
#include "view.h"

// Названия переходов (два падежа для красивых сообщений)
const char* WALK_NAMES[WALK_COUNT] = {"северный", "восточный", "южный", "западный"};
const char* WALK_TO[WALK_COUNT] = {"северному", "восточному", "южному", "западному"};

// Случайное событие с вероятностью percent процентов
int Chance(int percent) {
    return rand() % 100 < percent;
}

// Подсчет инвариантов
void InvariantViolation(Crossroad* cr, const char* what) {
    cr->stats.violations++;
    EventOut(C_RED, "НАРУШЕНИЕ ИНВАРИАНТА: %s", what);
}

// Создание перекрестка
void CrossroadInit(Crossroad* cr, Config* cfg) {
    memset(cr, 0, sizeof(*cr));
    cr->cfg = cfg;
    for(int i = 0; i < FLOW_COUNT; ++i)
        FlowInit(&cr->flows[i], cfg->queueCapacity);
    for(int w = 0; w < WALK_COUNT; ++w)
        CrosswalkInit(&cr->walks[w], cfg->queueCapacity);
    ControllerInit(cr);
}

// Прибытие новых автомобилей
void CarArrivalStep(Crossroad* cr) {
    const Config* cfg = cr->cfg;
    Stats* st = &cr->stats;
    // автомобили: с каждого направления не больше одного за такт
    for(int d = 0; d < DIR_COUNT; ++d) {
        if(!Chance(cfg->carRate[d])) continue;
        // случайный манёвр среди разрешённых на этом направлении
        int turns[TURN_COUNT], n = 0;
        for(int t = 0; t < TURN_COUNT; ++t)
            if(cfg->allowed[FlowIndex(d, t)]) turns[n++] = t;
        if(n == 0) continue;

        Vehicle v;
        v.id = ++cr->nextId;
        v.flow = FlowIndex(d, turns[rand() % n]);
        v.arrivalT = cr->t;
        v.emergency = Chance(cfg->emergencyRate);
        st->carsArrived++;
        if(v.emergency) st->emergencyArrived++;

        Flow* f = &cr->flows[v.flow];
        const char* who = v.emergency ? "ЭКСТРЕННОЕ авто" : "Авто";
        if(f->size >= f->capacity) {
            // переполнение очереди надо считать как затор
            st->carsRefused++;
            EventOut(C_RED, "%s #%d прибыло %s (%s): очередь %s заполнена - ОТКАЗ, затор",
                     who, v.id, DIR_FROM[d], TURN_NAMES[FlowTurn(v.flow)], FlowName(v.flow));
            continue;
        }
        // экстренная машина встаёт в начало очереди
        int front = v.emergency && cfg->emergencyPriority;
        if(front) FlowPushFront(f, v);
        else      FlowPush(f, v);
        EventOut(v.emergency ? C_PURPLE : NULL, "%s #%d прибыло %s (%s) - %s очереди %s: %d/%d",
                 who, v.id, DIR_FROM[d], TURN_NAMES[FlowTurn(v.flow)],
                 front ? "В НАЧАЛО" : "в конец", FlowName(v.flow), f->size, f->capacity);
    }
}

void PedArrivalStep(Crossroad* cr) {
    const Config* cfg = cr->cfg;
    Stats* st = &cr->stats;
    // у каждого перехода не больше одного за такт
    for(int w = 0; w < WALK_COUNT; ++w) {
        if(!Chance(cfg->pedRate)) continue;
        Pedestrian p = { ++cr->nextId, cr->t };
        Crosswalk* cw = &cr->walks[w];
        st->pedsArrived++;
        if(cw->size >= cw->capacity) {
            st->pedsRefused++;
            EventOut(C_RED, "Пешеход #%d подошёл к %s переходу: мест нет - ОТКАЗ", p.id, WALK_TO[w]);
            continue;
        }
        CrosswalkPush(cw, p);
        if(!IsPedGreen(cr) && !cr->ctrl.pedRequest) {
            cr->ctrl.pedRequest = 1;
            EventOut(C_CYAN, "Пешеход #%d подошёл к %s переходу и НАЖАЛ КНОПКУ вызова",
                     p.id, WALK_TO[w]);
        } else {
            EventOut(NULL, "Пешеход #%d подошёл к %s переходу и ждёт (ждут: %d)",
                     p.id, WALK_TO[w], cw->size);
        }
    }
}

// Завершение участником проезда/перехода
void LeaveStep(Crossroad* cr, int isPed) {
    Zone* z = &cr->zone;
    for(int k = 0; k < z->count; ) {
        Occupant* o = &z->items[k];
        if(o->leaveT > cr->t || (isPed >= 0 && o->isPed != isPed)) { ++k; continue; } // ещё едет/идёт
        if(o->isPed)
            EventOut(NULL, "Пешеход #%d завершил переход (%s переход)", o->id, WALK_NAMES[o->where]);
        else
            EventOut(NULL, "Авто #%d проехало перекрёсток (%s)", o->id, FlowName(o->where));
        z->items[k] = z->items[--z->count]; // удаление: на его место ставим последнего
    }
}

// Поместить участника в конфликтную зону
static void ZoneAdd(Crossroad* cr, int isPed, int id, int where) {
    cr->zone.items[cr->zone.count++] = (Occupant){isPed, id, where, cr->t + cr->cfg->crossTime};
}

// Въезд в зону участников с разрешающим сигналом
void EnterDirection(Crossroad* cr, int dir) {
    Stats* st = &cr->stats;
    Zone* z = &cr->zone;

    if(IsPedGreen(cr)) {
        // Все ожидающие на этом переходе могут начать переход
        Crosswalk* cw = &cr->walks[dir];
        while(cw->size > 0 && z->count < MAX_ZONE) {
            Pedestrian p = CrosswalkPop(cw);
            int delay = cr->t - p.arrivalT;
            st->pedsPassed++;
            st->pedDelay += delay;
            if(delay > st->pedMaxDelay) st->pedMaxDelay = delay;
            ZoneAdd(cr, 1, p.id, dir);
            EventOut(C_GREEN, "Пешеход #%d: зелёный, начал переход (%s переход), ждал %d такт.",
                     p.id, WALK_NAMES[dir], delay);
        }
        return;
    }
    // C каждой полосы с зелёным въезжает машина
    for(int turn = 0; turn < TURN_COUNT; ++turn) {
        int i = FlowIndex(dir, turn);
        Flow* f = &cr->flows[i];
        if(f->size == 0 || z->count >= MAX_ZONE) continue;
        if(!IsGreen(cr, i)) continue; // инвариант: только на зелёный
        Vehicle v = FlowPop(f);
        int delay = cr->t - v.arrivalT;
        st->carsPassed++;
        st->carDelay += delay;
        if(delay > st->carMaxDelay) st->carMaxDelay = delay;
        if(v.emergency) {
            st->emergencyPassed++;
            st->emergencyDelay += delay;
        }
        ZoneAdd(cr, 0, v.id, i);
        EventOut(v.emergency ? C_PURPLE : C_GREEN,
                 "%s #%d: разрешено движение, въезжает (%s), ждало %d такт.; очередь %s: %d/%d",
                 v.emergency ? "ЭКСТРЕННОЕ авто" : "Авто", v.id, FlowName(i), delay,
                 FlowName(i), f->size, f->capacity);
    }
}

// Проверка инвариантов
void CheckInvariants(Crossroad* cr) {
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
                return;
            }
            if(!x->isPed && cfg->conflict[x->where][y->where]) {
                snprintf(msg, sizeof(msg), "в зоне конфликтующие машины #%d и #%d", x->id, y->id);
                InvariantViolation(cr, msg);
            }
        }
    // Машина находится только в одном месте
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

// Один такт
void CrossroadStep(Crossroad* cr) {
    CarArrivalStep(cr);
    PedArrivalStep(cr);
    LeaveStep(cr, -1);
    ControllerStep(cr);
    for(int d = 0; d < DIR_COUNT; ++d) EnterDirection(cr, d);
    CheckInvariants(cr);
}
