//  Светофор - когда закончить фазу и переходный интервал

#include <stdio.h>
#include <string.h>

#include "crossroad.h"
#include "view.h"

// Горит ли зелёный полосе
int IsGreen(const Crossroad* cr, int flow) {
    const Phase* ph = &cr->cfg->phases[cr->ctrl.phase];
    return cr->ctrl.state == GREEN && ph->flows[flow] && cr->cfg->allowed[flow];
}

// Горит ли зелёный пешеходам
int IsPedGreen(const Crossroad* cr) {
    return cr->ctrl.state == GREEN && cr->cfg->phases[cr->ctrl.phase].isPedestrian;
}

// Возвращает номер полосы где есть экстренная машина в начале очереди
static int EmergencyWaiting(const Crossroad* cr) {
    if(!cr->cfg->emergencyPriority) return -1;
    for(int i = 0; i < FLOW_COUNT; ++i) {
        const Vehicle* v = FlowAt(&cr->flows[i], 0);
        if(v && v->emergency) return i;
    }
    return -1;
}

// Сколько участников ждут фазу p
static int PhaseDemand(const Crossroad* cr, int p) {
    const Phase* ph = &cr->cfg->phases[p];
    int demand = 0;
    if(ph->isPedestrian) {
        if(cr->ctrl.pedRequest) // пешеходная фаза нужна только по кнопке
            for(int w = 0; w < WALK_COUNT; ++w) demand += cr->walks[w].size;
    } else {
        for(int i = 0; i < FLOW_COUNT; ++i)
            if(ph->flows[i] && cr->cfg->allowed[i]) demand += cr->flows[i].size;
    }
    return demand;
}

// Сколько тактов ждёт первый участник фазы p
static int PhaseWait(const Crossroad* cr, int p) {
    const Phase* ph = &cr->cfg->phases[p];
    int wait = 0;
    if(ph->isPedestrian) {
        for(int w = 0; w < WALK_COUNT; ++w) {
            const Pedestrian* front = CrosswalkFront(&cr->walks[w]);
            if(front && cr->t - front->arrivalT > wait) wait = cr->t - front->arrivalT;
        }
    } else {
        for(int i = 0; i < FLOW_COUNT; ++i) {
            const Vehicle* front = FlowAt(&cr->flows[i], 0);
            if(ph->flows[i] && front && cr->t - front->arrivalT > wait) wait = cr->t - front->arrivalT;
        }
    }
    return wait;
}

// Ждёт ли кто-нибудь другую фазу, чтобы просто так
// не переключать зеленый если на других направлениях никого нет
static int OtherDemand(const Crossroad* cr) {
    for(int p = 0; p < cr->cfg->phaseCount; ++p)
        if(p != cr->ctrl.phase && PhaseDemand(cr, p) > 0) return 1;
    return 0;
}

// Где какой свет светофора
static const char* PhaseFlows(const Config* cfg, int p) {
    static char buf[512];
    const Phase* ph = &cfg->phases[p];
    if(ph->isPedestrian) return "все пешеходные переходы (машинам красный)";
    int len = 0;
    buf[0] = '\0';
    for(int i = 0; i < FLOW_COUNT; ++i)
        if(ph->flows[i] && cfg->allowed[i])
            len += snprintf(buf + len, sizeof(buf) - len, "%s%s", len ? ", " : "", FlowName(i));
    return buf;
}

// Включение фазы p, загорается зеленый
static void StartPhase(Crossroad* cr, int p) {
    Controller* c = &cr->ctrl;
    // новая фаза только при пустой зоне и после интервала
    if(cr->zone.count > 0)
        InvariantViolation(cr, "фаза включается при занятой конфликтной зоне");
    if(c->state == CLEARANCE && c->timer < cr->cfg->clearance)
        InvariantViolation(cr, "фаза включается до окончания переходного интервала");

    c->state = GREEN;
    c->phase = p;
    c->timer = 0;
    if(cr->cfg->phases[p].isPedestrian) c->pedRequest = 0; // пешеход пройден
    cr->stats.phaseSwitches++;

    EventOut(C_GREEN, "Начало фазы %s. ЗЕЛЕНЫЙ: %s", cr->cfg->phases[p].name,
             PhaseFlows(cr->cfg, p));
    EventOut(NULL, "Состояние сигналов: остальным - КРАСНЫЙ");
}

// Завершение текущей фазы, начало переходного интервала
static void EndPhase(Crossroad* cr, const char* reason) {
    Controller* c = &cr->ctrl;
    const char* name = cr->cfg->phases[c->phase].name;
    EventOut(C_YELLOW, "Фаза %s завершена через %d тактов (%s)", name, c->timer, reason);
    EventOut(C_YELLOW, "Переходный интервал %d тактов: ЖЕЛТЫЙ, новых участников не пускаем",
             cr->cfg->clearance);
    c->state = CLEARANCE; // с этого момента IsGreen() == 0 для всех
    c->timer = 0;
}

// Пора ли завершать текущую фазу.
static const char* ShouldEndPhase(const Crossroad* cr) {
    const Config* cfg = cr->cfg;
    const Controller* c = &cr->ctrl;
    if(c->timer < cfg->minGreen) return NULL; // минимальную длительность выдерживаем всегда

    // экстренный транспорт
    int ef = EmergencyWaiting(cr);
    if(ef >= 0 && !IsGreen(cr, ef)) return "приоритет экстренному транспорту";

    if(cfg->phases[c->phase].isPedestrian) return "время пешеходной фазы истекло";
    if(cfg->strategy == STRATEGY_FIXED)
        return c->timer >= cfg->maxGreen ? "истекло время фазы (фиксированный цикл)" : NULL;

    // Если больше никто не ждёт, то продлеваем зеленый
    if(!OtherDemand(cr)) return NULL;
    if(PhaseDemand(cr, c->phase) == 0) return "очереди фазы пусты, ждут другие направления";
    if(c->timer >= cfg->maxGreen) return "достигнута максимальная длительность";
    return NULL;
}

// Выбор следующей фазы
static int ChooseNextPhase(Crossroad* cr) {
    const Config* cfg = cr->cfg;
    const Controller* c = &cr->ctrl;
    int n = cfg->phaseCount;

    // Если какая-то фаза ждёт слишком долго, она включается первой
    int starving = -1, longest = 0;
    for(int k = 1; k < n; ++k) {
        int p = (c->phase + k) % n;
        int wait = PhaseWait(cr, p);
        if(PhaseDemand(cr, p) > 0 && wait >= cfg->starvationLimit && wait > longest) {
            longest = wait;
            starving = p;
        }
    }
    if(starving >= 0) {
        EventOut(C_PURPLE, "Защита от голодания: фаза %s ждёт уже %d тактов - включается вне очереди",
                 cfg->phases[starving].name, longest);
        return starving;
    }
    // Экстренный транспорт, ближайшая фаза
    int ef = EmergencyWaiting(cr);
    if(ef >= 0) {
        for(int k = 1; k <= n; ++k) {
            int p = (c->phase + k) % n;
            if(cfg->phases[p].flows[ef]) {
                EventOut(C_PURPLE, "Приоритет экстренному транспорту (%s): следующая фаза %s",
                         FlowName(ef), cfg->phases[p].name);
                return p;
            }
        }
    }
    // Фаза с наибольшим весом = сколько ждут + максимальное ожидание
    if(cfg->strategy == STRATEGY_ADAPTIVE) {
        int best = -1, bestWeight = 0;
        for(int k = 1; k < n; ++k) {
            int p = (c->phase + k) % n;
            int demand = PhaseDemand(cr, p);
            int weight = demand > 0 ? demand + PhaseWait(cr, p) : 0;
            if(weight > bestWeight) { bestWeight = weight; best = p; }
        }
        if(best >= 0) {
            EventOut(NULL, "Адаптивный выбор: фаза %s (вес %d)", cfg->phases[best].name, bestWeight);
            return best;
        }
    }
    // Следующая по кругу
    for(int k = 1; k <= n; ++k) {
        int p = (c->phase + k) % n;
        if(cfg->phases[p].isPedestrian && !c->pedRequest) continue;
        return p;
    }
    return c->phase;
}

void ControllerInit(Crossroad* cr) {
    memset(&cr->ctrl, 0, sizeof(cr->ctrl));
    StartPhase(cr, 0); // сразу включаем первую фазу
    cr->ctrl.timer = -1;
}

// Один такт
void ControllerStep(Crossroad* cr) {
    Controller* c = &cr->ctrl;
    c->timer++;
    if(c->state == GREEN) {
        const char* reason = ShouldEndPhase(cr);
        if(reason) EndPhase(cr, reason);
        return;
    }
    // Переходный интервал: ждём его окончания
    if(c->timer < cr->cfg->clearance) return;
    // ждём, пока все покинут зону
    if(cr->zone.count > 0) {
        if(c->timer == cr->cfg->clearance)
            EventOut(C_YELLOW, "Интервал истёк, но в зоне ещё %d участн. - ждём освобождения",
                     cr->zone.count);
        return;
    }
    StartPhase(cr, ChooseNextPhase(cr));
}

// Цвет сигнала для рисунка
char ControllerSignal(const Crossroad* cr, int flow) {
    if(IsGreen(cr, flow)) return 'G';
    // в переходный интервал потокам завершённой фазы горит жёлтый
    if(cr->ctrl.state == CLEARANCE && cr->cfg->phases[cr->ctrl.phase].flows[flow]) return 'Y';
    return 'R';
}

char ControllerPedSignal(const Crossroad* cr) {
    if(IsPedGreen(cr)) return 'G';
    if(cr->ctrl.state == CLEARANCE && cr->cfg->phases[cr->ctrl.phase].isPedestrian) return 'Y';
    return 'R';
}
