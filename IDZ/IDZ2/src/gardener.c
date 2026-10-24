#include "garden.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>

int synchronization_init(Simulation *s) {
    int error = pthread_mutex_init(&s->mutex, NULL);
    if (error) { errno = error; return -1; }
    for (int id = 0; id < 2; ++id) {
#ifdef USE_CONDVAR
        error = pthread_cond_init(&s->changed[id], NULL);
#else
        error = sem_init(&s->changed[id], 0, 0) == 0 ? 0 : errno;
#endif
        if (error) {
            for (int previous = 0; previous < id; ++previous) {
#ifdef USE_CONDVAR
                pthread_cond_destroy(&s->changed[previous]);
#else
                sem_destroy(&s->changed[previous]);
#endif
            }
            pthread_mutex_destroy(&s->mutex);
            errno = error;
            return -1;
        }
    }
    return 0;
}

void synchronization_destroy(Simulation *s) {
    for (int id = 0; id < 2; ++id) {
#ifdef USE_CONDVAR
        pthread_cond_destroy(&s->changed[id]);
#else
        sem_destroy(&s->changed[id]);
#endif
    }
    pthread_mutex_destroy(&s->mutex);
}

/* Изменение карты будит ожидающих. Порядок действий потоков не задаётся. */
void wake_waiters(Simulation *s) {
    ++s->revision;
    for (int id = 0; id < 2; ++id) {
        if (s->actors[id].state != WAITING && !s->stopped) continue;
#ifdef USE_CONDVAR
        int error = pthread_cond_signal(&s->changed[id]);
#else
        int error = sem_post(&s->changed[id]) == 0 ? 0 : errno;
#endif
        if (error) { s->failed = true; s->stopped = true; }
        if (s->actors[id].state == WAITING) s->actors[id].state = READY;
    }
}

/* На входе и выходе мьютекс захвачен. Предикат защищает и от ложных пробуждений. */
static void wait_for_change(Simulation *s, int id) {
    unsigned seen = s->revision;
    while (!s->stopped && seen == s->revision) {
#ifdef USE_CONDVAR
        int error = pthread_cond_wait(&s->changed[id], &s->mutex);
#else
        pthread_mutex_unlock(&s->mutex);
        int error;
        do { error = sem_wait(&s->changed[id]); } while (error != 0 && errno == EINTR);
        if (error != 0) error = errno;
        pthread_mutex_lock(&s->mutex);
#endif
        if (error) { s->failed = true; s->stopped = true; wake_waiters(s); }
    }
}

void gardener_init(Simulation *s, int id) {
    Gardener *a = &s->actors[id];
    *a = (Gardener){.simulation = s, .id = id, .position = s->cfg.start[id], .destination = -1};
    const Garden *g = &s->garden;
    int order[MAX_CELLS], count = 0;
    if (id == 0 || !s->cfg.columns) {
        for (int step = 0; step < g->rows; ++step) {
            int row = id == 0 ? step : g->rows - 1 - step;
            for (int k = 0; k < g->cols; ++k) {
                int col = (step + id) % 2 ? g->cols - 1 - k : k;
                order[count++] = row * g->cols + col;
            }
        }
    } else {
        for (int step = 0; step < g->cols; ++step) {
            int col = g->cols - 1 - step;
            for (int k = 0; k < g->rows; ++k)
                order[count++] = (step % 2 ? k : g->rows - 1 - k) * g->cols + col;
        }
    }
    int offset = 0;
    while (order[offset] != a->position) ++offset;
    for (int i = 0; i < count; ++i) a->route[i] = order[(offset + i) % count];
}

void *gardener_run(void *argument) {
    Gardener *a = argument;
    Simulation *s = a->simulation;
    Garden *g = &s->garden;
    pthread_mutex_lock(&s->mutex);
    while (!s->stopped) {
        if (s->actors[0].processed + s->actors[1].processed == g->reachable_count) {
            s->stopped = true;
            break;
        }
        while (a->cursor < g->rows * g->cols) {
            int cell = a->route[a->cursor];
            const char *reason = g->cells[cell] == '#' ? "препятствие" :
                                 g->cells[cell] == '+' ? "уже обработано" :
                                 !g->reachable[a->id][cell] ? "недостижима из его старта" : NULL;
            if (!reason) break;
            event(s, a->id, "пропустил (%d,%d): %s", cell / g->cols, cell % g->cols, reason);
            ++a->cursor;
        }
        if (a->cursor == g->rows * g->cols) break;
        int target = a->route[a->cursor];
        bool processing = a->position == target;
        int next = processing ? target : garden_next_step(s, a->id, target);
        if (next == -1) {
            if (a->state != WAITING)
                event(s, a->id, "ожидает путь к (%d,%d)", target / g->cols, target % g->cols);
            a->state = WAITING;
            State other = s->actors[1 - a->id].state;
            if (other == WAITING || other == FINISHED) {
                s->result = 2;
                s->stopped = true;
                break;
            }
            wait_for_change(s, a->id);
            continue;
        }
        a->state = processing ? PROCESSING : MOVING;
        if (processing) {
            event(s, a->id, "начал обработку (%d,%d)", target / g->cols, target % g->cols);
        } else {
            a->destination = next;
            event(s, a->id, "начал переход (%d,%d) -> (%d,%d)", a->position / g->cols,
                  a->position % g->cols, next / g->cols, next % g->cols);
        }
        /* Ожидание длительности действия выполняется БЕЗ мьютекса: второй поток работает. */
        pthread_mutex_unlock(&s->mutex);
        bool completed = action_pause(s, processing ? s->cfg.work : s->cfg.move[a->id]);
        pthread_mutex_lock(&s->mutex);
        if (!completed || s->stopped) break;
        if (processing) {
            assert(g->cells[a->position] == '.');
            g->cells[a->position] = '+';
            ++a->processed;
            ++a->cursor;
            event(s, a->id, "закончил обработку (%d,%d)", target / g->cols, target % g->cols);
        } else {
            a->position = a->destination;
            a->destination = -1;
            event(s, a->id, "закончил переход в (%d,%d)", next / g->cols, next % g->cols);
        }
        a->state = READY;
        assert(a->position != s->actors[1 - a->id].position);
        wake_waiters(s);
        if (s->failed) s->stopped = true;
    }
    a->position = a->destination = -1;
    a->state = FINISHED;
    ++s->finished;
    event(s, a->id, "закончил работу и освободил клетки");
    wake_waiters(s);
    pthread_mutex_unlock(&s->mutex);
    return NULL;
}
