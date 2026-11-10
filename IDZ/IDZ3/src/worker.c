#include "garden.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* Спим без общей блокировки. Короткие части позволяют быстро заметить остановку. */
static int action_pause(Ipc *ipc, int units) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    long long until = (long long)now.tv_sec * 1000000000LL + now.tv_nsec +
                      (long long)units * ipc->state.cfg.unit_ms * 1000000LL;
    while (!interrupted) {
        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
        long long remaining = until - ((long long)now.tv_sec * 1000000000LL + now.tv_nsec);
        if (remaining <= 0) break;
        struct timespec pause = {0, remaining < 20000000L ? (long)remaining : 20000000L};
        while (nanosleep(&pause, &pause) < 0) {
            if (errno != EINTR) return -1;
            if (interrupted) return 0;
        }
        if (ipc_lock(ipc) < 0) return -1;
        bool stopped = ipc->state.stopped;
        if (ipc_unlock(ipc) < 0) return -1;
        if (stopped) break;
    }
    return 0;
}

typedef struct {
    Status kind;                         /* MOVING или PROCESSING. */
    int target, units;
} Action;

static Action begin_action(Ipc *ipc, int id, int next) {
    State *s = &ipc->state;
    Actor *a = &s->actors[id];
    a->status = next == a->position ? PROCESSING : MOVING;
    if (a->status == PROCESSING)
        event(ipc, id, "начал обработку (%d,%d)", next / s->cfg.cols, next % s->cfg.cols);
    else {
        a->destination = next;
        event(ipc, id, "начал переход (%d,%d) -> (%d,%d)", a->position / s->cfg.cols,
              a->position % s->cfg.cols, next / s->cfg.cols, next % s->cfg.cols);
    }
    return (Action){a->status, next, a->status == PROCESSING ? s->cfg.work : s->cfg.move[id]};
}

static void complete_action(Ipc *ipc, int id, Action action) {
    State *s = &ipc->state;
    Actor *a = &s->actors[id];
    if (action.kind == PROCESSING) {
        s->cells[action.target] = '+'; ++a->processed;
        event(ipc, id, "закончил обработку (%d,%d)", action.target / s->cfg.cols, action.target % s->cfg.cols);
    } else {
        a->position = action.target; a->destination = -1;
        event(ipc, id, "закончил переход в (%d,%d)", action.target / s->cfg.cols, action.target % s->cfg.cols);
    }
    a->status = READY;
    ipc_notify(ipc);
}

int worker_run(Ipc *ipc, int id) {
    if (ipc_lock(ipc) < 0) return 1;
    State *s = &ipc->state;
    if (s->actors[id].pid || s->actors[id].status == FINISHED || s->stopped) {
        int code = s->stopped ? 0 : 1;
        ipc_unlock(ipc);
        dprintf(STDERR_FILENO, "Место садовника %d уже занято или работа завершена.\n", id + 1);
        return code;
    }
    if (ipc->log_fd < 0) ipc->log_fd = open(s->log_path, O_WRONLY | O_APPEND);
    if (ipc->log_fd < 0) {
        s->failed = s->stopped = true; ipc_notify(ipc); ipc_unlock(ipc); return 1;
    }
    int route[CELLS], cursor = 0;
    make_route(s, id, route);
    Actor *a = &s->actors[id];
    a->pid = getpid();
    event(ipc, id, "подключился");
    ipc_notify(ipc);
    /* Первый участник ждёт подключения второго, далее оба действуют независимо. */
    while (!s->stopped && !interrupted && !s->actors[1 - id].pid) {
        a->status = WAITING;
        if (ipc_wait(ipc, id) < 0 || ipc_lock(ipc) < 0) return 1;
    }
    if (!s->stopped) a->status = READY;
    while (!s->stopped && !interrupted) {
        if (s->actors[0].processed + s->actors[1].processed == s->reachable_count) {
            s->stopped = true; break;
        }
        while (cursor < s->cfg.rows * s->cfg.cols) {
            int cell = route[cursor];
            const char *reason = s->cells[cell] == '#' ? "препятствие" : s->cells[cell] == '+' ?
                "уже обработано" : !s->reachable[id][cell] ? "недостижима из его старта" : NULL;
            if (!reason) break;
            event(ipc, id, "пропустил (%d,%d): %s", cell / s->cfg.cols, cell % s->cfg.cols, reason);
            ++cursor;
        }
        if (cursor == s->cfg.rows * s->cfg.cols) break;
        int target = route[cursor];
        int next = a->position == target ? target : next_step(s, id, target);
        if (next == -1) {
            event(ipc, id, "ожидает путь к (%d,%d)", target / s->cfg.cols, target % s->cfg.cols);
            a->status = WAITING;
            Status other = s->actors[1 - id].status;
            if (other == WAITING || other == FINISHED) { s->result = 2; s->stopped = true; break; }
            if (ipc_wait(ipc, id) < 0 || ipc_lock(ipc) < 0) return 1;
            continue;
        }
        Action action = begin_action(ipc, id, next);
        if (ipc_unlock(ipc) < 0 || action_pause(ipc, action.units) < 0 || ipc_lock(ipc) < 0) return 1;
        if (s->stopped || interrupted) break;
        complete_action(ipc, id, action);
        if (action.kind == PROCESSING) ++cursor;
    }
    if (interrupted) { s->stopped = true; s->result = 128 + interrupted; }
    a->position = a->destination = -1; a->status = FINISHED;
    ++s->finished;
    event(ipc, id, "закончил работу и освободил клетки");
    ipc_notify(ipc);
    int result = s->failed ? 1 : 0;
    if (ipc_unlock(ipc) < 0) result = 1;
    return result;
}
