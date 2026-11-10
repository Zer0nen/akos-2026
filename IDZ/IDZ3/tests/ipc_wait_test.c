#include "../src/garden.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Подменяются только системные ошибки в тестовом файле, не в коде программы. */
enum { NONE, WAIT_ERROR, LOCK_ERROR, RECEIVE_ERROR, SEND_ERROR };
static int fault;
static sem_t *mutex, *changed;
int __real_sem_timedwait(sem_t *, const struct timespec *);
ssize_t __real_mq_timedreceive(mqd_t, char *, size_t, unsigned *, const struct timespec *);
int __real_mq_timedsend(mqd_t, const char *, size_t, unsigned, const struct timespec *);

int __wrap_sem_timedwait(sem_t *sem, const struct timespec *until) {
    if (sem == changed && fault == WAIT_ERROR) { errno = EIO; return -1; }
    if (sem == mutex && fault == LOCK_ERROR) { errno = ETIMEDOUT; return -1; }
    if (sem == changed && (fault == LOCK_ERROR || fault == RECEIVE_ERROR)) return 0;
    return __real_sem_timedwait(sem, until);
}
ssize_t __wrap_mq_timedreceive(mqd_t queue, char *data, size_t size, unsigned *priority,
                             const struct timespec *until) {
    if (fault == RECEIVE_ERROR) { errno = EIO; return -1; }
    return __real_mq_timedreceive(queue, data, size, priority, until);
}
int __wrap_mq_timedsend(mqd_t queue, const char *data, size_t size, unsigned priority,
                      const struct timespec *until) {
    if (fault == SEND_ERROR) { errno = EIO; return -1; }
    return __real_mq_timedsend(queue, data, size, priority, until);
}

static int run_case(Mode mode, int error, bool stopped) {
    State state = {.magic = MAGIC, .owner = getpid(), .reachable_count = 2,
                   .cfg = {.rows = 1, .cols = 2}};
    state.cells[0] = state.cells[1] = '.';
    for (int id = 0; id < 2; ++id) {
        state.actors[id].position = id;
        state.actors[id].destination = -1;
    }
    char name[40];
    snprintf(name, sizeof name, "wait_%ld_%d_%d_%d", (long)getpid(), mode, error, stopped);
    Ipc ipc;
    if (ipc_open(&ipc, mode, name, &state) < 0) { perror("test ipc_open"); return 1; }
    if (ipc_lock(&ipc) < 0) { ipc_close(&ipc); return 1; }
    mutex = ipc.mutex; changed = ipc.changed[0];
    ipc.state.stopped = stopped;
    ipc.state.actors[0].status = WAITING;
    pid_t notifier = -1;
    if (error == NONE && !stopped) {
        notifier = fork();
        if (notifier == 0) {
            ipc.owner = false;
            if (ipc_lock(&ipc) < 0) _exit(1);
            ipc_notify(&ipc);
            _exit(ipc_unlock(&ipc) < 0);
        }
        if (notifier < 0) { ipc_unlock(&ipc); ipc_close(&ipc); return 1; }
    }
    fault = error;
    int result = ipc_wait(&ipc, 0), saved = errno;
    fault = NONE;
    if (notifier > 0) {
        int status;
        if (waitpid(notifier, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) result = -2;
    }
    /* На любом выходе можно захватить семафор, а в очереди сохранено состояние. */
    int value = -1;
    bool unlocked = sem_getvalue(ipc.mutex, &value) == 0 && value == 1 && sem_trywait(ipc.mutex) == 0;
    if (unlocked) sem_post(ipc.mutex);
    struct mq_attr attr = {0};
    bool message_ok = mode != QUEUE || (mq_getattr(ipc.queue, &attr) == 0 &&
                      attr.mq_curmsgs == (error == SEND_ERROR ? 0 : 1));
    bool state_ok = true;
    if (unlocked && message_ok && error != SEND_ERROR) {
        state_ok = ipc_lock(&ipc) == 0;
        if (state_ok) {
            state_ok = ipc.state.owner == state.owner && !memcmp(ipc.state.cells, state.cells, 2);
            if (ipc_unlock(&ipc) < 0) state_ok = false;
        }
    }
    bool ok = unlocked && message_ok && state_ok && result == (error == NONE ? 0 : -1) &&
              (error == NONE || saved == (error == LOCK_ERROR ? ETIMEDOUT : EIO));
    ipc_close(&ipc);
    if (!ok) fprintf(stderr, "mode=%d fault=%d stopped=%d result=%d errno=%d unlocked=%d message=%d\n",
                     mode, error, stopped, result, saved, unlocked, message_ok);
    return !ok;
}
int main(void) {
    for (Mode mode = FORK_NAMED; mode <= QUEUE; ++mode) {
        if (run_case(mode, WAIT_ERROR, false) || run_case(mode, LOCK_ERROR, false) ||
            run_case(mode, NONE, true) || run_case(mode, NONE, false)) return 1;
        if (mode == QUEUE && (run_case(mode, RECEIVE_ERROR, false) || run_case(mode, SEND_ERROR, false))) return 1;
    }
    puts("ipc_wait: success and error exits release the lock; errno/state checked");
    return 0;
}
