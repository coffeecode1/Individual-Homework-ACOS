#ifndef GATE_H
#define GATE_H

//  Gate - счётный семафор, через который потоки модели передают друг другу
//  разрешения ("можно начинать такт", "можно въезжать", "я закончил").
//  Две реализации с одинаковым интерфейсом:
//    gate_sem.c  - на POSIX-семафорах (основная программа crossroad-mt);
//    gate_cond.c - на мьютексе и условной переменной (программа crossroad-mt-cv,
//                  собирается с -DGATE_COND).

#include <pthread.h>
#include <semaphore.h>

typedef struct Gate {
#ifdef GATE_COND
    pthread_mutex_t lock; // защищает count
    pthread_cond_t  cond; // ждём, пока count станет > 0
    unsigned count; // число накопленных разрешений
#else
    sem_t* sem; // рабочий семафор
    sem_t  storage; // память под безымянный семафор (Linux)
#endif
} Gate;

// Создание с начальным числом разрешений value. Возвращает 0 при успехе.
int  GateInit(Gate* g, unsigned value);
void GateDestroy(Gate* g);

// Выдать одно разрешение (V / sem_post)
void GatePost(Gate* g);

// Забрать одно разрешение, при необходимости дождавшись его (P / sem_wait)
void GateWait(Gate* g);

// Название используемых синхропримитивов (для вывода)
const char* GateKind(void);

#endif // GATE_H
