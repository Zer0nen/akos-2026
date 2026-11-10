#include "garden.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void name_for(const Ipc *ipc, const char *suffix, char name[128]) {
    snprintf(name, 128, "%s_%s", ipc->base, suffix);
}
static struct timespec deadline(void) {
    struct timespec time;
    clock_gettime(CLOCK_REALTIME, &time); ++time.tv_sec;
    return time;
}
static int wait_sem(sem_t *sem) {
    struct timespec until = deadline();
    int result;
    do { result = sem_timedwait(sem, &until); } while (result < 0 && errno == EINTR);
    return result;
}
int ipc_lock(Ipc *ipc) {
    if (wait_sem(ipc->mutex) < 0) return -1;
    if (ipc->mode == QUEUE) {
        struct timespec until = deadline();
        ssize_t size;
        do { size = mq_timedreceive(ipc->queue, (char *)&ipc->state, sizeof(State), NULL, &until); }
        while (size < 0 && errno == EINTR);
        if (size != sizeof(State)) {
            int saved = size < 0 ? errno : EPROTO;
            sem_post(ipc->mutex); errno = saved;
            return -1;
        }
    } else ipc->state = ipc->memory->state;
    return 0;
}
int ipc_unlock(Ipc *ipc) {
    assert(state_valid(&ipc->state));
    int result = 0, saved = 0;
    if (ipc->mode == QUEUE) {
        struct timespec until = deadline();
        do { result = mq_timedsend(ipc->queue, (char *)&ipc->state, sizeof(State), 0, &until); }
        while (result < 0 && errno == EINTR);
        if (result < 0) saved = errno;
    } else ipc->memory->state = ipc->state;
    if (sem_post(ipc->mutex) < 0) { if (!saved) saved = errno; result = -1; }
    if (saved) errno = saved;
    return result;
}
void ipc_notify(Ipc *ipc) {
    ++ipc->state.revision;
    for (int id = 0; id < 2; ++id) {
        Actor *a = &ipc->state.actors[id];
        if (a->status != WAITING && !ipc->state.stopped) continue;
        if (sem_post(ipc->changed[id]) < 0) ipc->state.failed = ipc->state.stopped = true;
        if (a->status == WAITING) a->status = READY;
    }
}
int ipc_wait(Ipc *ipc, int id) {
    unsigned seen = ipc->state.revision;
    while (!ipc->state.stopped && seen == ipc->state.revision && !interrupted) {
        if (ipc_unlock(ipc) < 0) return -1;
        int result;
        struct timespec until = deadline();
        do { result = sem_timedwait(ipc->changed[id], &until); } while (result < 0 && errno == EINTR && !interrupted);
        int wait_error = result < 0 ? errno : 0;
        if (ipc_lock(ipc) < 0) return -1;
        if (wait_error && wait_error != EINTR && wait_error != ETIMEDOUT) {
            ipc_unlock(ipc); errno = wait_error;
            return -1;
        }
        if (!process_alive(ipc->state.owner)) {
            ipc->state.failed = ipc->state.stopped = true; ipc_notify(ipc);
        }
    }
    return ipc_unlock(ipc);
}

int ipc_open(Ipc *ipc, Mode mode, const char *session, const State *initial) {
    *ipc = (Ipc){.mode = mode, .owner = initial != NULL, .fd = -1, .log_fd = -1,
                 .queue = (mqd_t)-1, .mutex = SEM_FAILED, .changed = {SEM_FAILED, SEM_FAILED}};
    if (!*session || strlen(session) > 40) { errno = EINVAL; return -1; }
    for (const unsigned char *p = (const unsigned char *)session; *p; ++p)
        if (!isalnum(*p) && *p != '_') { errno = EINVAL; return -1; }
    snprintf(ipc->base, sizeof ipc->base, "/akos3_%u_%d_%s", (unsigned)getuid(), mode, session);
    char name[128];
    name_for(ipc, "state", name);
    int flags = O_RDWR | (ipc->owner ? O_CREAT | O_EXCL : 0);
    if (mode == QUEUE) {
        struct mq_attr attr = {.mq_maxmsg = 1, .mq_msgsize = sizeof(State)};
        ipc->queue = mq_open(name, flags, 0600, &attr);
        if (ipc->queue == (mqd_t)-1) goto fail;
        ipc->resource_created = ipc->owner;
        if (mq_getattr(ipc->queue, &attr) < 0 || attr.mq_msgsize != sizeof(State)) { errno = EINVAL; goto fail; }
    } else {
        ipc->fd = shm_open(name, flags, 0600);
        if (ipc->fd < 0) goto fail;
        ipc->resource_created = ipc->owner;
        if (ipc->owner && ftruncate(ipc->fd, sizeof(SharedMemory)) < 0) goto fail;
        struct stat info;
        if (fstat(ipc->fd, &info) < 0 || info.st_size != sizeof(SharedMemory)) { errno = EINVAL; goto fail; }
        ipc->memory = mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, ipc->fd, 0);
        if (ipc->memory == MAP_FAILED) { ipc->memory = NULL; goto fail; }
    }
    if (mode == FORK_UNNAMED) {
        ipc->mutex = &ipc->memory->mutex;
        for (int id = 0; id < 2; ++id) ipc->changed[id] = &ipc->memory->changed[id];
        sem_t *sems[] = {ipc->mutex, ipc->changed[0], ipc->changed[1]};
        for (int i = 0; i < 3; ++i) {
            if (sem_init(sems[i], 1, 0) < 0) goto fail;  /* pshared=1: семафоры между процессами. */
            ++ipc->initialized;
        }
    } else {
        sem_t **sems[] = {&ipc->mutex, &ipc->changed[0], &ipc->changed[1]};
        const char *suffix[] = {"lock", "wake0", "wake1"};
        for (int i = 0; i < 3; ++i) {
            name_for(ipc, suffix[i], name);
            *sems[i] = sem_open(name, ipc->owner ? O_CREAT | O_EXCL : 0, 0600, 0);
            if (*sems[i] == SEM_FAILED) goto fail;
        }
    }
    if (ipc->owner) {
        ipc->state = *initial;
        if (ipc_unlock(ipc) < 0) goto fail;  /* Публикуем готовое состояние и открываем доступ. */
    } else {
        if (ipc_lock(ipc) < 0) goto fail;
        bool valid = ipc->state.magic == MAGIC && process_alive(ipc->state.owner);
        if (ipc_unlock(ipc) < 0) goto fail;
        if (!valid) { errno = EINVAL; goto fail; }
    }
    return 0;
fail: {
    int saved = errno;
    ipc_close(ipc); errno = saved;
    return -1;
}}

void ipc_close(Ipc *ipc) {
    char name[128];
    sem_t *sems[] = {ipc->mutex, ipc->changed[0], ipc->changed[1]};
    const char *suffix[] = {"lock", "wake0", "wake1"};
    for (int i = 0; i < 3; ++i) {
        if (sems[i] == SEM_FAILED) continue;
        if (ipc->mode == FORK_UNNAMED) {
            if (ipc->owner && i < ipc->initialized) sem_destroy(sems[i]);
        } else {
            sem_close(sems[i]);
            if (ipc->owner) { name_for(ipc, suffix[i], name); sem_unlink(name); }
        }
    }
    if (ipc->memory) munmap(ipc->memory, sizeof(SharedMemory));
    if (ipc->fd >= 0) close(ipc->fd);
    if (ipc->queue != (mqd_t)-1) mq_close(ipc->queue);
    if (ipc->owner && ipc->resource_created) {
        name_for(ipc, "state", name);
        if (ipc->mode == QUEUE) mq_unlink(name); else shm_unlink(name);
    }
    if (ipc->log_fd >= 0) close(ipc->log_fd);
}
