//  Многопоточная версия модели перекрёстка (второе ДЗ, вариант 39).
//  Параметры, журнал и вывод - те же, что в первом ДЗ.

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>

#include "../config.h"
#include "../view.h"
#include "sim.h"

// Флаг остановки по Ctrl+C, как в первом дз
static volatile sig_atomic_t Stop = 0;

// Обработчик SIGINT и SIGTERM.
static void OnSignal(int sig) {
    (void)sig;
    Stop = 1;
}

int main(int argc, char** argv) {
    Config cfg;
    static Sim sim; // большая структура - не на стеке

    // Параметры
    int rc = ConfigParseArgs(&cfg, argc, argv);
    if(rc > 0) {
        return 0;
    } else if(rc < 0) {
        return 1;
    }
    if(ConfigValidate(&cfg) != 0) {
        dprintf(2, "Неверные параметры\n");
        return 1;
    }

    // Обработка прерывания
    struct sigaction sa;
    sa.sa_handler = OnSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Журнал
    ViewOpenLog(cfg.logFile);
    ConfigPrint(&cfg, 1);
    ConfigPrint(&cfg, ViewLogFd());
    ViewSetMode(cfg.visual);
    ViewSetTime(0);

    // SIGINT/SIGTERM блокируем - сигнал получит только главный поток.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
    int started = SimStart(&sim, &cfg);
    pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
    if(started != 0) {
        ViewCloseLog();
        return 1;
    }
    dprintf(1, "Многопоточный режим: %d служебных потоков + поток на каждого участника (%s)\n",
            WORKER_COUNT, GateKind());

    // Главный поток, генерит такты
    char* reason = "прервано пользователем";
    Crossroad* cr = &sim.cr;
    while(!Stop) {
        pthread_mutex_lock(&sim.lock);
        ViewSetTime(cr->t);
        ViewTick(cr); // рисунок перекрёстка (в режиме -q ничего не делает)
        pthread_mutex_unlock(&sim.lock);

        SimStep(&sim); // стадии такта выполняют другие потоки

        pthread_mutex_lock(&sim.lock);
        cr->t++;
        int passed = CrossroadPassed(cr);
        pthread_mutex_unlock(&sim.lock);
        if(!cfg.infinite) {
            if(cfg.participants > 0 && passed >= cfg.participants) {
                reason = "пропущено заданное число участников";
                break;
            }
            if(cfg.duration > 0 && cr->t >= cfg.duration) {
                reason = "истёк период наблюдения";
                break;
            }
        }
        if(cfg.delayMs > 0) usleep((useconds_t)cfg.delayMs * 1000);
    }

    // Остановка: будим и завершаем все потоки, а потом итоги
    SimStop(&sim);
    ViewStats(cr, reason);
    SimThreadStats(&sim);
    dprintf(1, "Журнал сохранён в файл: %s\n", cfg.logFile);
    ViewCloseLog();
    return 0;
}
