#include "garden.h"
#include <assert.h>
#include <stddef.h>

void gardener_init(Gardener *a, const Garden *g, const Config *cfg, int id) {
    *a = (Gardener){.id = id, .position = cfg->start[id], .destination = -1};
    int route[MAX_CELLS], count = 0;
    if (id == 0 || !cfg->route2_columns) {
        for (int step = 0; step < g->rows; ++step) {
            int row = id == 0 ? step : g->rows - 1 - step;
            for (int k = 0; k < g->cols; ++k) {
                int col = (step + id) % 2 ? g->cols - 1 - k : k;
                route[count++] = row * g->cols + col;
            }
        }
    } else {
        for (int step = 0; step < g->cols; ++step) {
            int col = g->cols - 1 - step;
            for (int k = 0; k < g->rows; ++k) {
                int row = step % 2 ? k : g->rows - 1 - k;
                route[count++] = row * g->cols + col;
            }
        }
    }
    /* При другом старте начинаем с него, сохраняя порядок змейки. */
    int offset = 0;
    while (route[offset] != a->position) ++offset;
    for (int i = 0; i < count; ++i) a->route[i] = route[(offset + i) % count];
}

void gardener_complete(Gardener *a, Garden *g, int tick, Output *out) {
    if ((a->state != MOVING && a->state != PROCESSING) || tick < a->due_tick) return;
    if (a->state == MOVING) {
        a->position = a->destination;
        a->destination = -1;
        output_event(out, "[t=%d] Садовник %d закончил переход в (%d,%d).\n",
                     tick, a->id + 1, a->position / g->cols, a->position % g->cols);
    } else {
        assert(g->cells[a->position] == '.');
        g->cells[a->position] = '+';
        ++a->processed;
        ++a->cursor;
        output_event(out, "[t=%d] Садовник %d закончил обработку (%d,%d).\n",
                     tick, a->id + 1, a->position / g->cols, a->position % g->cols);
    }
    a->state = READY;
}

void gardener_finish(Gardener *a, int tick, Output *out) {
    if (a->state == FINISHED) return;
    a->position = a->destination = -1;
    a->state = FINISHED;
    output_event(out, "[t=%d] Садовник %d закончил работу и освободил клетки.\n", tick, a->id + 1);
}

bool gardener_step(Gardener *a, Garden *g, const Gardener actors[2],
                   const Config *cfg, int tick, Output *out) {
    if (a->state == FINISHED || a->state == MOVING || a->state == PROCESSING) return false;
    while (a->cursor < g->rows * g->cols) {
        int target = a->route[a->cursor];
        int row = target / g->cols, col = target % g->cols;
        const char *reason = NULL;
        if (g->cells[target] == '#') reason = "препятствие";
        else if (g->cells[target] == '+') reason = "уже обработано";
        else if (!g->reachable[a->id][target]) reason = "недостижима из его старта";
        if (reason) {
            output_event(out, "[t=%d] Садовник %d пропустил (%d,%d): %s.\n",
                         tick, a->id + 1, row, col, reason);
            ++a->cursor;
            a->state = READY;
            continue;
        }
        if (a->position == target) {
            a->state = PROCESSING;
            a->due_tick = tick + cfg->work_ticks;
            output_event(out, "[t=%d] Садовник %d начал обработку (%d,%d), до t=%d.\n",
                         tick, a->id + 1, row, col, a->due_tick);
            return true;
        }
        int next = garden_next_step(g, actors, a->id, target, cfg->detours);
        if (next == -1) {
            if (a->state != WAITING)
                output_event(out, "[t=%d] Садовник %d ожидает путь к (%d,%d).\n",
                             tick, a->id + 1, row, col);
            a->state = WAITING;
            return false;
        }
        a->destination = next;
        a->state = MOVING;
        a->due_tick = tick + cfg->move_ticks[a->id];
        output_event(out, "[t=%d] Садовник %d начал переход (%d,%d) -> (%d,%d), до t=%d.\n",
                     tick, a->id + 1, a->position / g->cols, a->position % g->cols,
                     next / g->cols, next % g->cols, a->due_tick);
        return true;
    }
    gardener_finish(a, tick, out);
    return true;
}
