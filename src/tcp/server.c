#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../view.h"

#define MAX_CLIENTS 64
enum { NEW = -2, WATCH = -1 };

typedef struct Peer {
    int fd, role;
    char line[NET_LINE];
    size_t used;
    long long connected;
} Peer;

typedef struct Server {
    Crossroad cr;
    Peer peers[MAX_CLIENTS];
    int workers[DIR_COUNT]; // номер соединения для каждой стороны
    int answered[DIR_COUNT];
    int turn[DIR_COUNT], emergency[DIR_COUNT], ped[DIR_COUNT];
    int waiting, failed;
    long long deadline; // сколько ждём ответ на уже отправленный такт
} Server;

static volatile sig_atomic_t Interrupted;
static void OnSignal(int sig) { Interrupted = sig; }

static void Drop(Server* server, int index) {
    Peer* peer = &server->peers[index];
    if(peer->role >= 0) {
        server->workers[peer->role] = -1;
        // уже полученный ответ сохраняем, второй раз его применять не надо.
        EventOut(C_YELLOW, "СЕТЬ: сторона %d отключилась, ждём подключения", peer->role);
    }
    close(peer->fd);
    peer->fd = -1;
}

static void Send(Server* server, int index, const char* text) {
    // сокеты неблокирующие и медленный наблюдатель не задерживает модель
    if(index >= 0 && server->peers[index].fd != -1 && NetSend(server->peers[index].fd, text) == -1)
        Drop(server, index);
}

static void Broadcast(Server* server, const char* text) {
    for(int i = 0; i < MAX_CLIENTS; ++i)
        if(server->peers[i].fd != -1 && server->peers[i].role == WATCH)
            Send(server, i, text);
}

static void Event(const char* msg, void* data) {
    Server* server = data;
    char line[NET_LINE];
    snprintf(line, sizeof(line), "[t=%04d] %s\n", server->cr.t, msg);
    Broadcast(server, line);
    // Обычный вывод тоже нужен для журнала сервера.
    ViewSetEventSink(NULL, NULL);
    EventOut(NULL, "%s", msg);
    ViewSetEventSink(Event, server);
}

// отправляем весь перекрёсток, чтобы можно было смотреть только наблюдателя
static void Snapshot(Server* server, int index) {
    Crossroad* cr = &server->cr;
    char line[NET_LINE];
    snprintf(line, sizeof(line), "СОСТОЯНИЕ t=%d, фаза %s, %s, таймер %d, зона %d, кнопка %d\n",
             cr->t, cr->cfg->phases[cr->ctrl.phase].name,
             cr->ctrl.state == GREEN ? "зелёный" : "переходный интервал",
             cr->ctrl.timer, cr->zone.count, cr->ctrl.pedRequest);
    Send(server, index, line);
    for(int d = 0; d < DIR_COUNT; ++d) {
        int f = FlowIndex(d, 0);
        snprintf(line, sizeof(line),
                 "СТОРОНА %d: Л %c/%d, П %c/%d, Пр %c/%d; пешеходы %c/%d\n",
                 d, ControllerSignal(cr, f), cr->flows[f].size,
                 ControllerSignal(cr, f + 1), cr->flows[f + 1].size,
                 ControllerSignal(cr, f + 2), cr->flows[f + 2].size,
                 ControllerPedSignal(cr), cr->walks[d].size);
        Send(server, index, line);
    }
    Stats* st = &cr->stats;
    snprintf(line, sizeof(line),
             "ИТОГ: авто %d/%d, пешеходы %d/%d (пропущено/прибыло), отказы %d, "
             "ср. ожидание %.2f/%.2f, макс. %d/%d, экстренные %d/%d, фазы %d, нарушения %d\n",
             st->carsPassed, st->carsArrived, st->pedsPassed, st->pedsArrived,
             st->carsRefused + st->pedsRefused,
             st->carsPassed ? (double)st->carDelay / st->carsPassed : 0,
             st->pedsPassed ? (double)st->pedDelay / st->pedsPassed : 0,
             st->carMaxDelay, st->pedMaxDelay, st->emergencyPassed,
             st->emergencyArrived, st->phaseSwitches, st->violations);
    Send(server, index, line);
}

// Сначала клиент выбирает роль и потом стороны присылают прибытия
static int Handle(Server* server, int index) {
    Peer* peer = &server->peers[index];
    Config* cfg = server->cr.cfg;
    int dir, turn, emergency, ped, tick, used = 0;
    if(peer->role == NEW) {
        if(strcmp(peer->line, "WATCH") == 0) {
            peer->role = WATCH;
            Snapshot(server, index); // новый наблюдатель сразу видит текущую ситуацию
            return 0;
        }
        if(sscanf(peer->line, "WORK %d%n", &dir, &used) != 1 || peer->line[used] ||
           dir < 0 || dir >= DIR_COUNT || server->workers[dir] != -1) return -1;
        peer->role = dir;
        server->workers[dir] = index;
        int allowed = 0;
        for(int t = 0; t < TURN_COUNT; ++t)
            if(cfg->allowed[FlowIndex(dir, t)]) allowed |= 1 << t;
        char line[128];
        snprintf(line, sizeof(line), "CONFIG %d %d %d %d %u\n",
                 cfg->carRate[dir], cfg->pedRate, cfg->emergencyRate, allowed, cfg->seed + dir);
        Send(server, index, line);
        EventOut(NULL, "СЕТЬ: подключилась сторона %d", dir);
        if(server->waiting && !server->answered[dir]) {
            // у остальных сторон ответы уже могут быть готовы
            snprintf(line, sizeof(line), "TICK %d\n", server->cr.t);
            Send(server, index, line);
            server->deadline = NetTime() + 5000;
        }
        return 0;
    }
    dir = peer->role;
    if(dir == WATCH || !server->waiting || server->answered[dir] ||
       sscanf(peer->line, "ARRIVE %d %d %d %d%n", &tick, &turn, &emergency, &ped, &used) != 4 ||
       peer->line[used] || tick != server->cr.t || turn < -1 || turn >= TURN_COUNT ||
       emergency < 0 || emergency > 1 || ped < 0 || ped > 1 ||
       (turn == -1 && emergency) || (turn >= 0 && !cfg->allowed[FlowIndex(dir, turn)])) return -1;
    server->turn[dir] = turn;
    server->emergency[dir] = emergency;
    server->ped[dir] = ped;
    server->answered[dir] = 1;
    return 0;
}

static int Ready(const Server* server, int answers) {
    for(int d = 0; d < DIR_COUNT; ++d)
        if(server->workers[d] == -1 || (answers && !server->answered[d])) return 0;
    return 1;
}

static void Tick(Server* server) {
    Crossroad* cr = &server->cr;
    // применяем ответы в порядке сторон, поэтому seed даёт одинаковый результат
    for(int d = 0; d < DIR_COUNT; ++d) {
        if(server->turn[d] >= 0) CarArrive(cr, d, server->turn[d], server->emergency[d]);
        if(server->ped[d]) PedArrive(cr, d);
    }
    LeaveStep(cr, -1);
    ControllerStep(cr);
    for(int d = 0; d < DIR_COUNT; ++d) EnterDirection(cr, d);
    CheckInvariants(cr);
    cr->t++;
    for(int i = 0; i < MAX_CLIENTS; ++i)
        if(server->peers[i].fd != -1 && server->peers[i].role == WATCH) Snapshot(server, i);
    for(int d = 0; d < DIR_COUNT; ++d) {
        int f = FlowIndex(d, 0);
        char line[128];
        snprintf(line, sizeof(line), "СТОРОНА %d t=%d: %c/%d %c/%d %c/%d, пешеходы %c/%d\n",
                 d, cr->t, ControllerSignal(cr, f), cr->flows[f].size,
                 ControllerSignal(cr, f + 1), cr->flows[f + 1].size,
                 ControllerSignal(cr, f + 2), cr->flows[f + 2].size,
                 ControllerPedSignal(cr), cr->walks[d].size);
        Send(server, server->workers[d], line);
    }
}

// проверяем всех сразу, чтобы новый наблюдатель не ждал конца модели
static void PollClients(Server* server, int listener) {
    struct pollfd fds[MAX_CLIENTS + 1] = {{ .fd = listener, .events = POLLIN }};
    for(int i = 0; i < MAX_CLIENTS; ++i)
        fds[i + 1] = (struct pollfd){ .fd = server->peers[i].fd, .events = POLLIN };
    int rc = poll(fds, MAX_CLIENTS + 1, 20);
    if(rc == -1) {
        if(errno != EINTR) server->failed = 1;
        return;
    }
    if(fds[0].revents & POLLIN) {
        int fd = accept(listener, NULL, NULL);
        if(fd >= 0) {
            int i = 0;
            while(i < MAX_CLIENTS && server->peers[i].fd != -1) ++i;
            if(i == MAX_CLIENTS || fcntl(fd, F_SETFL, O_NONBLOCK) == -1) close(fd);
            else server->peers[i] = (Peer){ .fd = fd, .role = NEW, .connected = NetTime() };
        }
    }
    for(int i = 0; i < MAX_CLIENTS; ++i) {
        Peer* peer = &server->peers[i];
        if(peer->fd == -1) continue;
        if(peer->role == NEW && NetTime() - peer->connected > 5000) { Drop(server, i); continue; }
        if(!(fds[i + 1].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        char buffer[NET_LINE];
        ssize_t count = recv(peer->fd, buffer, sizeof(buffer), 0);
        if(count == -1 && (errno == EAGAIN || errno == EINTR)) continue;
        if(count <= 0) { Drop(server, i); continue; }
        for(ssize_t j = 0; j < count && peer->fd != -1; ++j) {
            if(buffer[j] == '\n') {
                peer->line[peer->used] = '\0';
                if(Handle(server, i) == -1) {
                    Send(server, i, "ERROR неверная команда или занятая сторона\n");
                    if(peer->fd != -1) Drop(server, i);
                }
                peer->used = 0;
            } else if(buffer[j] == '\0' || peer->used + 1 == sizeof(peer->line)) Drop(server, i);
            else peer->line[peer->used++] = buffer[j];
        }
    }
}

int main(int argc, char** argv) {
    if(argc < 3) {
        dprintf(2, "Запуск: %s IP ПОРТ [параметры модели]\n", argv[0]);
        return 1;
    }
    const char* ip = argv[1];
    const char* port = argv[2];
    argv[2] = argv[0];
    Config cfg;
    int rc = ConfigParseArgs(&cfg, argc - 2, argv + 2);
    if(rc != 0) return rc < 0;
    if(ConfigValidate(&cfg) != 0) return 1;
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = { .sa_handler = OnSignal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    int listener = NetOpen(ip, port, 1);
    if(listener == -1) { perror("Сервер"); return 1; }
    if(ViewOpenLog(cfg.logFile) == -1) { close(listener); return 1; }
    Server server = {0};
    for(int i = 0; i < MAX_CLIENTS; ++i) server.peers[i].fd = -1;
    for(int d = 0; d < DIR_COUNT; ++d) server.workers[d] = -1;
    ViewSetMode(cfg.visual);
    ConfigPrint(&cfg, 1);
    ConfigPrint(&cfg, ViewLogFd());
    ViewSetEventSink(Event, &server);
    CrossroadInit(&server.cr, &cfg);
    dprintf(1, "TCP READY %s %s\n", ip, port);
    long long nextTick = 0;
    const char* reason = NULL;
    while(!Interrupted && !server.failed && !reason) {
        PollClients(&server, listener);
        if(server.failed) break;
        // выполняем такт, когда все четыре стороны прислали ответ
        if(server.waiting && Ready(&server, 1)) {
            Tick(&server);
            server.waiting = 0;
            nextTick = NetTime() + cfg.delayMs;
            if(!cfg.infinite && cfg.participants > 0 && CrossroadPassed(&server.cr) >= cfg.participants)
                reason = "пропущено заданное число участников";
            else if(!cfg.infinite && cfg.duration > 0 && server.cr.t >= cfg.duration)
                reason = "истёк период наблюдения";
        }
        if(!server.waiting && Ready(&server, 0) && NetTime() >= nextTick && !reason) {
            ViewSetTime(server.cr.t);
            ViewTick(&server.cr);
            char line[64];
            snprintf(line, sizeof(line), "TICK %d\n", server.cr.t);
            server.waiting = 1;
            memset(server.answered, 0, sizeof(server.answered));
            for(int d = 0; d < DIR_COUNT; ++d) Send(&server, server.workers[d], line);
            server.deadline = NetTime() + 5000;
        }
        if(server.waiting && Ready(&server, 0) && NetTime() >= server.deadline) {
            // если клиент завис, освобождаем его сторону для нового запуска
            for(int d = 0; d < DIR_COUNT; ++d)
                if(!server.answered[d]) Drop(&server, server.workers[d]);
        }
    }
    if(server.failed) reason = "ошибка взаимодействия клиентов";
    if(!reason) reason = "прервано пользователем";
    char end[256];
    snprintf(end, sizeof(end), "END %s\n", reason);
    for(int i = 0; i < MAX_CLIENTS; ++i) {
        if(server.peers[i].fd == -1) continue;
        if(server.peers[i].role == WATCH) Snapshot(&server, i);
        Send(&server, i, end);
        if(server.peers[i].fd != -1) close(server.peers[i].fd);
    }
    ViewSetEventSink(NULL, NULL);
    ViewStats(&server.cr, reason);
    ViewCloseLog();
    close(listener);
    return server.failed;
}
