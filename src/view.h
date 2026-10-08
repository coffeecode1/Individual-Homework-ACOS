#ifndef VIEW_H
#define VIEW_H

//  Отображение: сообщения о событиях, рисунок перекрёстка в терминале, журнал.
#include "crossroad.h"

// ANSI-коды цветов терминала
#define C_RESET  "\x1b[0m"
#define C_RED    "\x1b[1;31m"
#define C_GREEN  "\x1b[1;32m"
#define C_YELLOW "\x1b[1;33m"
#define C_BLUE   "\x1b[1;34m"
#define C_PURPLE "\x1b[1;35m"
#define C_CYAN   "\x1b[1;36m"
#define C_GRAY   "\x1b[0;37m"

// Открытие журнала. Возвращает 0 при успехе.
int ViewOpenLog(const char* fileName);
void ViewCloseLog(void);

// Задание режима вывода (1 - с рисунком перекрёстка) и текущего такта
void ViewSetMode(int visual);
void ViewSetTime(int t);

// Сообщение о событии: печатается на экран (цветом color, может быть NULL)
// и записывается в журнал без цвета.
void EventOut(const char* color, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));

// Рисунок перекрёстка (вызывается в начале такта, события печатаются под ним)
void ViewTick(const Crossroad* cr);

// Итоговая статистика: на экран и в журнал
void ViewStats(const Crossroad* cr, const char* reason);

// Дескриптор журнала (для записи в него из других модулей)
int ViewLogFd(void);

#endif // VIEW_H
