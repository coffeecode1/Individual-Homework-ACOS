#ifndef CONTROLLER_H
#define CONTROLLER_H

#include "config.h"

typedef enum State {
    GREEN, // горит зелёный для потоков
    CLEARANCE // переходный интервал, никого не пускаем
} State;

typedef struct Controller {
    State state; // текущее состояние
    int phase; // текущая фаза
    int timer; // сколько тактов длится текущая фаза
    int pedRequest; // нажата кнопка вызова пешеходного сигнала
} Controller;

struct Crossroad;

// Включается первая фаза
void ControllerInit(struct Crossroad* cr);

// Один такт работы
void ControllerStep(struct Crossroad* cr);

// Горит ли зелёный
int IsGreen(const struct Crossroad* cr, int flow);
int IsPedGreen(const struct Crossroad* cr);

// Цвет сигнала для рисунка
char ControllerSignal(const struct Crossroad* cr, int flow);
char ControllerPedSignal(const struct Crossroad* cr);

#endif // CONTROLLER_H
