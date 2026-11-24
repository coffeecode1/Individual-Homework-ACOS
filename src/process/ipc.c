#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

const char* const RoleNames[ROLE_COUNT] = {
    "cars", "pedestrians", "controller", "north", "east", "south", "west"
};
static const char* const FifoNames[] = {"request", "response"};

static void Name(char name[32], const Ipc* ipc, int index) {
    snprintf(name, 32, "/cr39-%ld-%d", (long)ipc->owner, index);
}

static int OpenMemory(Ipc* ipc, int create) {
    char name[32];
    Name(name, ipc, SEM_COUNT);
    int fd = shm_open(name, create ? O_RDWR | O_CREAT | O_EXCL : O_RDWR, 0600);
    if(fd == -1) return -1;
    void* map = MAP_FAILED;
    if(!create || ftruncate(fd, sizeof(Shared)) == 0)
        map = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int error = errno;
    close(fd);
    if(map != MAP_FAILED) {
        ipc->shared = map;
        return 0;
    }
    if(create) shm_unlink(name);
    return IpcError(error);
}

static int OpenFifos(Ipc* ipc, int create) {
    // Координатор открывает оба конца, чтобы не ждать запуска исполнителей
    for(int i = REQUEST; i <= RESPONSE; ++i) {
        if(create && mkfifoat(ipc->dir, FifoNames[i], 0600) == -1) return -1;
        for(int side = READ_END; side <= WRITE_END; ++side) {
            if(!create && side != (i == REQUEST ? READ_END : WRITE_END)) continue;
            int flags = side == READ_END ? O_RDONLY : O_WRONLY;
            int fd = openat(ipc->dir, FifoNames[i], flags | O_NONBLOCK);
            if(fd == -1) return -1;
            ipc->fd[i][side] = fd;
        }
    }
    return 0;
}

static int OpenResources(Ipc* ipc, int create) {
    int rc = ipc->mode == INDEPENDENT_FIFO ? OpenFifos(ipc, create) : OpenMemory(ipc, create);
    if(rc == -1) return -1;
    int count = ipc->dir == -1 ? CLAIM : SEM_COUNT;
    for(int i = 0; i < count; ++i) {
        sem_t* sem;
        if(ipc->mode == FORK_UNNAMED) {
#ifdef __linux__
            sem = &ipc->shared->sem[i];
            if(sem_init(sem, 1, 0) == -1) return -1;
#else
            return IpcError(ENOSYS);
#endif
        } else {
            char name[32];
            Name(name, ipc, i);
            sem = create ? sem_open(name, O_CREAT | O_EXCL, 0600, i >= CLAIM)
                         : sem_open(name, 0);
            if(sem == SEM_FAILED) return -1;
        }
        ipc->sem[ipc->semCount++] = sem;
    }
    return 0;
}

int IpcCreate(Ipc* ipc, enum Mode mode, const char* session) {
    *ipc = (Ipc){ .mode = mode, .owner = getpid(), .dir = -1, .fd = {{-1, -1}, {-1, -1}} };
    if(session) {
        if(mkdir(session, 0700) == -1) return -1;
        ipc->session = session;
        ipc->dir = open(session, O_RDONLY | O_DIRECTORY);
        if(ipc->dir == -1) return -1;
    }
    return OpenResources(ipc, 1);
}

int IpcPublish(const Ipc* ipc) {
    int fd = openat(ipc->dir, "starting", O_WRONLY | O_CREAT | O_EXCL, 0600);
    if(fd == -1) return -1;
    int rc = dprintf(fd, "%ld %d\n", (long)ipc->owner, ipc->mode);
    int error = errno;
    close(fd);

    // имя ready появляется, когда файл уже полностью записан
    if(rc < 0) return IpcError(error);
    return renameat(ipc->dir, "starting", ipc->dir, "ready");
}

int IpcConnect(Ipc* ipc, const char* session, enum Role role) {
    *ipc = (Ipc){ .dir = open(session, O_RDONLY | O_DIRECTORY), .fd = {{-1, -1}, {-1, -1}} };
    if(ipc->dir == -1) return -1;
    int fd = openat(ipc->dir, "ready", O_RDONLY);
    if(fd == -1) return -1;
    FILE* info = fdopen(fd, "r");
    if(!info) return -1;
    long owner;
    int mode;
    int fields = fscanf(info, "%ld %d", &owner, &mode);
    fclose(info);
    if(fields != 2 || owner <= 1 || (long)(pid_t)owner != owner ||
       (mode != INDEPENDENT_SHM && mode != INDEPENDENT_FIFO)) return IpcError(EINVAL);

    ipc->owner = (pid_t)owner;
    ipc->mode = mode;
    if(kill(ipc->owner, 0) == -1 || OpenResources(ipc, 0) == -1) return -1;
    return sem_trywait(ipc->sem[CLAIM + role]); // одну роль занимает один процесс
}

void IpcClose(Ipc* ipc) {
    for(int i = 0; i < ipc->semCount; ++i) {
        if(ipc->mode == FORK_UNNAMED) {
#ifdef __linux__
            sem_destroy(ipc->sem[i]);
#endif
        } else {
            char name[32];
            Name(name, ipc, i);
            sem_unlink(name);
            sem_close(ipc->sem[i]);
        }
    }
    if(ipc->shared) {
        char name[32];
        Name(name, ipc, SEM_COUNT);
        munmap(ipc->shared, sizeof(Shared));
        shm_unlink(name);
    }
    if(ipc->dir != -1) {
        unlinkat(ipc->dir, "ready", 0);
        unlinkat(ipc->dir, "starting", 0);
        if(ipc->mode == INDEPENDENT_FIFO)
            for(int i = REQUEST; i <= RESPONSE; ++i) unlinkat(ipc->dir, FifoNames[i], 0);
        close(ipc->dir);
    }
    if(ipc->session) rmdir(ipc->session);
    // дескрипторы FIFO закроются при выходе из программы
}

static int64_t NowMs(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int IpcWait(sem_t* sem, int seconds, const volatile sig_atomic_t* interrupted) {
    // Проверяем флаг остановки, пока ждём ответ
    int64_t deadline = NowMs() + seconds * 1000;
    do {
        if(interrupted && *interrupted) return IpcError(EINTR);
        if(sem_trywait(sem) == 0) return 0;
        if(errno != EAGAIN && errno != EINTR) return -1;
        nanosleep(&(struct timespec){ .tv_nsec = 1000000 }, NULL);
    } while(NowMs() < deadline);
    return IpcError(ETIMEDOUT);
}

// Передаём пакет по частям, потому что может не уместиться
static int Transfer(int fd, void* data, size_t size, int writing) {
    char* bytes = data;
    struct pollfd event = { .fd = fd, .events = writing ? POLLOUT : POLLIN };
    while(size > 0) {
        int ready = poll(&event, 1, 5000);
        if(ready == 0) return IpcError(ETIMEDOUT);
        ssize_t count = ready == -1 ? -1 :
                        writing ? write(fd, bytes, size) : read(fd, bytes, size);
        if(count == 0) return IpcError(EPIPE);
        if(count == -1) {
            if(errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
        bytes += count;
        size -= (size_t)count;
    }
    return 0;
}

int FifoTransfer(const Ipc* ipc, enum Channel channel, Packet* packet, int writing) {
    int fd = ipc->fd[channel][writing ? WRITE_END : READ_END];
    if(Transfer(fd, packet, offsetof(Packet, events), writing) == -1) return -1;
    if(packet->eventBytes > sizeof(packet->events)) return IpcError(EPROTO);
    return Transfer(fd, packet->events, packet->eventBytes, writing);
}
