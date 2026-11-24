#include "ipc.h"

#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../view.h"

static volatile sig_atomic_t Interrupted;

static void OnSignal(int sig) { Interrupted = sig; }

static int StartWorkers(Ipc* ipc, pid_t children[ROLE_COUNT]) {

    // тут дети наследуют уже открытые семафоры и отображение разделяемой памяти
    for(int i = 0; i < ROLE_COUNT; ++i) {
        pid_t pid = fork();
        if(pid == -1) return -1;
        if(pid == 0) {
            int rc = WorkerRun(ipc, i);
            if(rc == -1) perror(RoleNames[i]);
            _exit(rc != 0);
        }
        children[i] = pid;
        dprintf(1, "%s PID %ld\n", RoleNames[i], (long)pid);
    }
    return 0;
}

static int WaitWorkers(Ipc* ipc, int seconds, const volatile sig_atomic_t* interrupted) {
    for(int i = 0; i < ROLE_COUNT; ++i)
        if(IpcWait(ipc->sem[READY], seconds, interrupted) == -1) return -1;
    return 0;
}

static int StopWorkers(Ipc* ipc, const pid_t children[ROLE_COUNT], int ready, int failed) {
    // сначала выдаём разрешения на остановку, потом будим всех остальных
    for(int i = 0; i < ROLE_COUNT; ++i) sem_post(ipc->sem[STOP]);
    for(int i = 0; i < ROLE_COUNT; ++i) sem_post(ipc->sem[i]);
    if(ipc->session) return ready && !failed ? WaitWorkers(ipc, 5, NULL) : 0;

    int error = 0;
    for(int i = 0; i < ROLE_COUNT && children[i]; ++i) {
        if(failed) kill(children[i], SIGTERM);
        int status;
        pid_t pid;
        do { pid = waitpid(children[i], &status, 0); } while(pid == -1 && errno == EINTR);
        if(pid == -1 || (!failed && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)))
            error = ECHILD;
    }
    return error ? IpcError(error) : 0;
}

// В каждый момент с пакетом работает один процесс
static int RunStage(Ipc* ipc, Packet* packet, enum Role role, int leave, Crossroad* cr) {
    packet->cr = *cr;
    packet->cr.cfg = NULL;
    packet->leave = leave;
    packet->eventBytes = 0;
    if(sem_post(ipc->sem[role]) == -1) return -1;
    // В FIFO сначала пишем задание, только потом читаем результат
    if(ipc->shared) {
        if(IpcWait(ipc->sem[DONE], 5, NULL) == -1) return -1;
    } else if(FifoTransfer(ipc, REQUEST, packet, 1) == -1 ||
              FifoTransfer(ipc, RESPONSE, packet, 0) == -1) return -1;
    if(packet->eventBytes > sizeof(packet->events) ||
       (packet->eventBytes && packet->events[packet->eventBytes - 1] != '\0'))
        return IpcError(EPROTO);
    Config* cfg = cr->cfg;
    *cr = packet->cr;
    cr->cfg = cfg;

    for(size_t offset = 0; offset < packet->eventBytes; ) {
        const char* msg = packet->events + offset;
        EventOut(NULL, "%s", msg);
        offset += strlen(msg) + 1;
    }
    return 0;
}

static int Step(Ipc* ipc, Packet* packet, Crossroad* cr) {
    // Прибытие, выезд, светофор, въезд с каждой стороны
    const struct { enum Role role; int leave; } jobs[] = {
        {CAR, 0}, {PED, 0}, {CAR, 1}, {PED, 1}, {LIGHT, 0},
        {NORTH, 0}, {EAST, 0}, {SOUTH, 0}, {WEST, 0}
    };
    for(size_t i = 0; i < sizeof(jobs) / sizeof(jobs[0]); ++i)
        if(RunStage(ipc, packet, jobs[i].role, jobs[i].leave, cr) == -1) return -1;
    CheckInvariants(cr);
    return 0;
}

int main(int argc, char** argv) {
    const enum Mode mode = PROCESS_MODE;
    const char* session = NULL;
    if(mode == INDEPENDENT_SHM || mode == INDEPENDENT_FIFO) {
        if(argc > 1 && argv[1][0] == '/') {
            session = argv[1];
            argv[1] = argv[0];
            ++argv;
            --argc;
        } else {
            dprintf(2, "Запуск: %s /путь/к/новому/каталогу [параметры модели]\n", argv[0]);
            if(argc != 2 || (strcmp(argv[1], "-h") && strcmp(argv[1], "--help"))) return 1;
        }
    }
    Config cfg;
    int rc = ConfigParseArgs(&cfg, argc, argv);
    if(rc != 0) return rc < 0;
    if(ConfigValidate(&cfg) != 0) return 1;

    struct sigaction sa = { .sa_handler = OnSignal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    Ipc ipc;
    if(IpcCreate(&ipc, mode, session) == -1) {
        perror("Создание IPC");
        IpcClose(&ipc);
        return 1;
    }
    if(ViewOpenLog(cfg.logFile) == -1) {
        IpcClose(&ipc);
        return 1;
    }

    const int outputs[] = {1, ViewLogFd()};
    for(int i = 0; i < 2; ++i) {
        dprintf(outputs[i], "Программа: %s\n", argv[0]);
        ConfigPrint(&cfg, outputs[i]);
    }
    ViewSetMode(cfg.visual);

    Crossroad cr;
    CrossroadInit(&cr, &cfg);
    pid_t children[ROLE_COUNT] = {0};
    if(session) dprintf(1, "Запустите 7 процессов crossroad-worker, каталог %s\n", session);
    int failed = (session ? IpcPublish(&ipc) : StartWorkers(&ipc, children)) == -1;
    int ready = !failed && WaitWorkers(&ipc, session ? 60 : 5, &Interrupted) == 0;
    if(!ready && !Interrupted) failed = 1;
    if(failed) perror("Запуск процессов");

    Packet local;
    Packet* packet = ipc.shared ? &ipc.shared->packet : &local;
    packet->cfg = cfg;
    const char* reason = NULL;
    while(!Interrupted && !failed && !reason) {
        ViewSetTime(cr.t);
        ViewTick(&cr);
        if(Step(&ipc, packet, &cr) == -1) {
            perror("Обмен с процессом");
            failed = 1;
            break;
        }
        cr.t++;
        if(!cfg.infinite && cfg.participants > 0 && CrossroadPassed(&cr) >= cfg.participants)
            reason = "пропущено заданное число участников";
        else if(!cfg.infinite && cfg.duration > 0 && cr.t >= cfg.duration)
            reason = "истёк период наблюдения";
        if(!reason && !Interrupted && cfg.delayMs > 0) usleep((useconds_t)cfg.delayMs * 1000);
    }

    if(StopWorkers(&ipc, children, ready, failed) == -1) {
        perror("Завершение процессов");
        failed = 1;
    }
    if(failed) reason = "ошибка взаимодействия процессов";
    ViewStats(&cr, reason ? reason : "прервано пользователем");
    ViewCloseLog();
    IpcClose(&ipc);
    return failed;
}
