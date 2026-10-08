#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#include "config.h"
#include "crossroad.h"
#include "view.h"

// Сделал свой обработчик, чтобы при прерывании программы она не завершалась моментально,
// а дорабатывала такт и выводила логи. Stop меняется в функции OnSignal, но сама эта функция
// срабатывает только при нажатии пользователем CTRL + C. Это SIGINT.
static volatile sig_atomic_t Stop = 0;

// Обработчик SIGINT.
static void OnSignal(int sig) {
    (void)sig;
    Stop = 1;
}

int main(int argc, char** argv) {
    Config cfg;
    Crossroad cr;

    // Параметры
    int rc = ConfigParseArgs(&cfg, argc, argv);
    if (rc > 0) {
        return 0;
    } else if (rc < 0){
        return 1;
    }
    if(ConfigValidate(&cfg) != 0) {
        dprintf(2, "Неверные параметры\n");
        return 1;
    }

    // Обработка прерывания
    struct sigaction sa;
    sa.sa_handler = OnSignal; // функция которую вызывается при получении сигнала
    sigemptyset(&sa.sa_mask); // это набор сигналов, которые ОС будет блокировать на время работы обработчика
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Журнал
    ViewOpenLog(cfg.logFile);
    ConfigPrint(&cfg, 1);
    ConfigPrint(&cfg, ViewLogFd());
    srand(cfg.seed);
    ViewSetMode(cfg.visual);
    ViewSetTime(0);
    CrossroadInit(&cr, &cfg);

    // Главный цикл процесса
    char* reason = "прервано пользователем";
    while(!Stop) {
        ViewSetTime(cr.t);
        ViewTick(&cr); // рисунок перекрёстка (в режиме -q ничего не делает)
        CrossroadStep(&cr); // один такт модели, события печатаются сразу
        cr.t++;
        if(!cfg.infinite) {
            if(cfg.participants > 0 && CrossroadPassed(&cr) >= cfg.participants) {
                reason = "пропущено заданное число участников";
                break;
            }
            if(cfg.duration > 0 && cr.t >= cfg.duration) {
                reason = "истёк период наблюдения";
                break;
            }
        }
        if(cfg.delayMs > 0) usleep((useconds_t)cfg.delayMs * 1000);
    }

    // Просмотр итогов
    ViewStats(&cr, reason);
    dprintf(1, "Журнал сохранён в файл: %s\n", cfg.logFile);
    ViewCloseLog();
    return 0;
}
