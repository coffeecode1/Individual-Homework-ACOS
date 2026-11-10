//  Вывод на экран и в журнал.

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "view.h"

static int  gLogFd = -1; // дескриптор журнала
static int  gVisual = 1; // режим вывода
static int  gColor = 1; // 1 - стандартный вывод - терминал, можно цвета
static struct {
    void (*write)(const char* msg, void* data);
    void* data;
} gEventSink;
static int  gTime = 0; // текущий такт (для подписи событий)

int ViewOpenLog(const char* fileName) {
    gLogFd = open(fileName, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(gLogFd == -1) {
        dprintf(2, "Ошибка: не удалось создать журнал '%s'\n", fileName);
        return -1;
    }
    return 0;
}

int ViewLogFd(void) { return gLogFd; }

void ViewSetTime(int t) { gTime = t; }

void ViewSetEventSink(void (*sink)(const char* msg, void* data), void* data) {
    gEventSink.write = sink;
    gEventSink.data = data;
}

void ViewCloseLog(void) {
    if(gLogFd != -1) close(gLogFd);
    gLogFd = -1;
}

void ViewSetMode(int visual) {
    gVisual = visual;
    gColor = isatty(1); // при выводе в файл/канал цвета не нужны
}

// Сообщение о событии
void EventOut(const char* color, const char* fmt, ...) {
    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if(gEventSink.write) {
        gEventSink.write(msg, gEventSink.data);
        return;
    }

    // В журнал - всегда и без цвета
    if(gLogFd != -1) dprintf(gLogFd, "[t=%04d] %s\n", gTime, msg);

    // На экран (цветом, если вывод идёт в терминал)
    if(gColor)
        dprintf(1, C_GRAY "[t=%04d]" C_RESET " %s%s%s\n", gTime,
                color ? color : "", msg, color ? C_RESET : "");
    else
        dprintf(1, "[t=%04d] %s\n", gTime, msg);
}


// Цветная "лампочка" светофора
static const char* Lamp(char signal) {
    switch(signal) {
    case 'G': return C_GREEN "●" C_RESET;
    case 'Y': return C_YELLOW "●" C_RESET;
    default:  return C_RED "●" C_RESET;
    }
}

// Клетка рисунка для одной полосы: "Л ● 3" (ширина 7 символов)
static void Cell(const Crossroad* cr, int dir, int turn, char* buf, size_t size) {
    static const char* labels[TURN_COUNT] = {"Л ", "П ", "Пр"};
    int flow = FlowIndex(dir, turn);
    if(!cr->cfg->allowed[flow]) {
        snprintf(buf, size, C_GRAY "%s ·  -" C_RESET, labels[turn]);
        return;
    }
    snprintf(buf, size, "%s %s%3d", labels[turn], Lamp(ControllerSignal(cr, flow)),
             cr->flows[flow].size);
}

// Рисунок перекрёстка: четыре подхода с сигналами и длинами очередей
static void DrawCross(const Crossroad* cr) {
    char n[TURN_COUNT][64], s[TURN_COUNT][64], w[TURN_COUNT][64], e[TURN_COUNT][64];
    for(int t = 0; t < TURN_COUNT; ++t) {
        Cell(cr, DIR_N, t, n[t], sizeof(n[t]));
        Cell(cr, DIR_S, t, s[t], sizeof(s[t]));
        Cell(cr, DIR_W, t, w[t], sizeof(w[t]));
        Cell(cr, DIR_E, t, e[t], sizeof(e[t]));
    }
    int cars = 0, peds = 0;
    for(int k = 0; k < cr->zone.count; ++k) {
        if(cr->zone.items[k].isPed) ++peds; else ++cars;
    }
    const char* side[TURN_COUNT] = {"", "  СЕВЕР", ""};
    const char* sideS[TURN_COUNT] = {"", "  ЮГ", ""};
    for(int t = 0; t < TURN_COUNT; ++t)
        dprintf(1, "                    │ %s │%s\n", n[t], side[t]);
    dprintf(1, "     ───────────────┘         └───────────────\n");
    dprintf(1, "      З %s        ЗОНА          В %s\n", w[0], e[0]);
    dprintf(1, "      З %s      авто:%3d        В %s\n", w[1], cars, e[1]);
    dprintf(1, "      З %s      пеш.:%3d        В %s\n", w[2], peds, e[2]);
    dprintf(1, "     ───────────────┐         ┌───────────────\n");
    for(int t = 0; t < TURN_COUNT; ++t)
        dprintf(1, "                    │ %s │%s\n", s[t], sideS[t]);
}

//------------------------------------------------------------------------------
void ViewTick(const Crossroad* cr) {
    if(!gVisual) return;
    const Controller* c = &cr->ctrl;
    const Config* cfg = cr->cfg;
    const Stats* st = &cr->stats;

    dprintf(1, "\x1b[2J\x1b[H"); // очистка экрана, курсор в левый верхний угол
    dprintf(1, C_CYAN "=== Регулируемый перекрёсток (вариант 39) | такт %d | %s ===" C_RESET "\n",
            cr->t, cfg->strategy == STRATEGY_FIXED ? "фиксированный цикл" : "адаптивное управление");
    if(c->state == GREEN)
        dprintf(1, "Контроллер: " C_GREEN "ЗЕЛЁНЫЙ" C_RESET ", фаза %s, идёт %d такт.\n",
                cfg->phases[c->phase].name, c->timer);
    else
        dprintf(1, "Контроллер: " C_YELLOW "ПЕРЕХОДНЫЙ ИНТЕРВАЛ" C_RESET " %d/%d (после фазы %s)\n",
                c->timer, cfg->clearance, cfg->phases[c->phase].name);
    dprintf(1, "\n");
    DrawCross(cr);
    dprintf(1, "\nПешеходы %s  ждут: С %d  В %d  Ю %d  З %d   кнопка вызова: %s\n",
            Lamp(ControllerPedSignal(cr)), cr->walks[0].size, cr->walks[1].size,
            cr->walks[2].size, cr->walks[3].size,
            c->pedRequest ? C_CYAN "НАЖАТА" C_RESET : "-");
    dprintf(1, "Прибыло: %d авто, %d пеш. | Пропущено: %d авто, %d пеш. | Отказов: %d | "
               "Нарушений инвариантов: %d\n",
            st->carsArrived, st->pedsArrived, st->carsPassed, st->pedsPassed,
            st->carsRefused + st->pedsRefused, st->violations);
    dprintf(1, C_GRAY "Л/П/Пр - налево/прямо/направо, число - длина очереди. Ctrl+C - остановка"
               C_RESET "\n\nСобытия такта:\n");
    // события такта напечатает EventOut сразу под рисунком
}

// Среднее значение
static double Avg(long sum, int count) {
    return count > 0 ? (double)sum / count : 0.0;
}

// Итоговая статистика в файл с дескриптором fd
static void StatsTo(int fd, const Crossroad* cr, const char* reason) {
    const Stats* st = &cr->stats;
    int carsWaiting = 0, pedsWaiting = 0;
    for(int i = 0; i < FLOW_COUNT; ++i) carsWaiting += cr->flows[i].size;
    for(int w = 0; w < WALK_COUNT; ++w) pedsWaiting += cr->walks[w].size;

    dprintf(fd, "\n================ ИТОГОВАЯ СТАТИСТИКА ================\n");
    dprintf(fd, "Причина завершения: %s\n", reason);
    dprintf(fd, "Модельное время: %d тактов, стратегия: %s, seed = %u\n", cr->t,
            cr->cfg->strategy == STRATEGY_FIXED ? "фиксированный цикл" : "адаптивная", cr->cfg->seed);
    dprintf(fd, "Автомобили: прибыло %d, пропущено %d, ждут в очередях %d, отказов %d\n",
            st->carsArrived, st->carsPassed, carsWaiting, st->carsRefused);
    dprintf(fd, "Средняя задержка автомобиля: %.2f такта (макс. %d)\n",
            Avg(st->carDelay, st->carsPassed), st->carMaxDelay);
    dprintf(fd, "Пешеходы: прибыло %d, пропущено %d, ждут %d, отказов %d, ср. ожидание %.2f (макс. %d)\n",
            st->pedsArrived, st->pedsPassed, pedsWaiting, st->pedsRefused,
            Avg(st->pedDelay, st->pedsPassed), st->pedMaxDelay);
    dprintf(fd, "Экстренный транспорт: прибыло %d, пропущено %d, ср. ожидание %.2f\n",
            st->emergencyArrived, st->emergencyPassed, Avg(st->emergencyDelay, st->emergencyPassed));
    dprintf(fd, "Включений фаз: %d\n", st->phaseSwitches);
    dprintf(fd, "Нарушений инвариантов: %d\n", st->violations);
    dprintf(fd, "=====================================================\n");
}

void ViewStats(const Crossroad* cr, const char* reason) {
    StatsTo(1, cr, reason);
    if(gLogFd != -1) StatsTo(gLogFd, cr, reason);
}
