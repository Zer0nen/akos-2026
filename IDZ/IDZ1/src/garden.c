#include "garden.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Соседи по стороне: вправо, вниз, влево, вверх. */
static int neighbours(const Garden *g, int cell, int result[4]) {
    int row = cell / g->cols, col = cell % g->cols, count = 0;
    if (col + 1 < g->cols) result[count++] = cell + 1;
    if (row + 1 < g->rows) result[count++] = cell + g->cols;
    if (col > 0) result[count++] = cell - 1;
    if (row > 0) result[count++] = cell - g->cols;
    return count;
}

/* Обычный обход в ширину отмечает клетки, достижимые из начальной позиции. */
static void mark_reachable(Garden *g, int id, int start) {
    int queue[MAX_CELLS], head = 0, tail = 0;
    queue[tail++] = start;
    g->reachable[id][start] = true;
    while (head < tail) {
        int next[4], count = neighbours(g, queue[head++], next);
        for (int i = 0; i < count; ++i) {
            int cell = next[i];
            if (g->cells[cell] != '#' && !g->reachable[id][cell]) {
                g->reachable[id][cell] = true;
                queue[tail++] = cell;
            }
        }
    }
}

int garden_load(Garden *g, Config *cfg, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("Открытие входного файла"); return -1; }
    FILE *file = fdopen(fd, "r");           /* libc читает из открытого дескриптора */
    if (!file) { perror("fdopen"); close(fd); return -1; }
    memset(g, 0, sizeof *g);
    int r1, c1, r2, c2, detours;
    char route;
    if (fscanf(file, "%d %d %d %d %d %c %d %d %d %d %d %d",
               &g->rows, &g->cols, &cfg->move_ticks[0], &cfg->move_ticks[1],
               &cfg->work_ticks, &route, &detours, &r1, &c1, &r2, &c2,
               &cfg->delay_ms) != 12) goto invalid;
    if (g->rows < 1 || g->rows > MAX_SIDE || g->cols < 1 || g->cols > MAX_SIDE ||
        g->rows * g->cols < 2 || (route != 'C' && route != 'R') ||
        (detours != 0 && detours != 1) || cfg->delay_ms < 0 || cfg->delay_ms > 1000 ||
        cfg->move_ticks[0] < 1 || cfg->move_ticks[0] > 100 ||
        cfg->move_ticks[1] < 1 || cfg->move_ticks[1] > 100 ||
        cfg->work_ticks < 1 || cfg->work_ticks > 100 ||
        r1 < 0 || r1 >= g->rows || c1 < 0 || c1 >= g->cols ||
        r2 < 0 || r2 >= g->rows || c2 < 0 || c2 >= g->cols) goto invalid;

    for (int row = 0; row < g->rows; ++row) {
        char line[MAX_SIDE + 1];
        if (fscanf(file, "%30s", line) != 1 || (int)strlen(line) != g->cols) goto invalid;
        for (int col = 0; col < g->cols; ++col) {
            if (line[col] != '.' && line[col] != '#') goto invalid;
            g->cells[row * g->cols + col] = line[col];
            if (line[col] == '.') ++g->open_count;
        }
    }
    char extra;
    if (fscanf(file, " %c", &extra) != EOF || ferror(file)) goto invalid;
    cfg->start[0] = r1 * g->cols + c1;
    cfg->start[1] = r2 * g->cols + c2;
    if (cfg->start[0] == cfg->start[1] || g->cells[cfg->start[0]] == '#' ||
        g->cells[cfg->start[1]] == '#') goto invalid;
    cfg->route2_columns = route == 'C';
    cfg->detours = detours != 0;
    if (fclose(file) != 0) { perror("Закрытие входного файла"); return -1; }
    for (int id = 0; id < 2; ++id) mark_reachable(g, id, cfg->start[id]);
    for (int i = 0; i < g->rows * g->cols; ++i)
        if (g->reachable[0][i] || g->reachable[1][i]) ++g->reachable_count;
    return 0;
invalid:
    dprintf(STDERR_FILENO, "Некорректные параметры или карта: %s. Формат описан в README.\n", path);
    fclose(file);
    return -1;
}

/* Во время перехода заняты и исходная клетка, и клетка назначения. */
static bool free_cell(const Garden *g, const Gardener actors[2], int id, int cell) {
    const Gardener *other = &actors[1 - id];
    return g->cells[cell] != '#' && cell != other->position &&
           !(other->state == MOVING && cell == other->destination);
}

int garden_next_step(const Garden *g, const Gardener actors[2],
                     int id, int target, bool detours) {
    int start = actors[id].position;
    if (!detours) {
        int next = start;
        if (target % g->cols != start % g->cols)
            next += target % g->cols > start % g->cols ? 1 : -1;
        else next += target / g->cols > start / g->cols ? g->cols : -g->cols;
        return free_cell(g, actors, id, next) ? next : -1;
    }

    /* При разрешённых обходах BFS находит путь в обход стен и второго участника. */
    int previous[MAX_CELLS], queue[MAX_CELLS], head = 0, tail = 0;
    for (int i = 0; i < g->rows * g->cols; ++i) previous[i] = -1;
    previous[start] = start;
    queue[tail++] = start;
    while (head < tail && previous[target] == -1) {
        int cell = queue[head++], next[4], count = neighbours(g, cell, next);
        for (int i = 0; i < count; ++i) {
            if (previous[next[i]] == -1 && free_cell(g, actors, id, next[i])) {
                previous[next[i]] = cell;
                queue[tail++] = next[i];
            }
        }
    }
    if (previous[target] == -1) return -1;
    int next = target;
    while (previous[next] != start) next = previous[next];
    return next;
}

static void print_border(const Garden *g, Output *out) {
    output_control(out, "\033[90m");
    output_print(out, "   +");
    for (int col = 0; col < g->cols; ++col) output_print(out, "---+");
    output_print(out, "\n");
    output_control(out, "\033[0m");
}

void garden_print(const Garden *g, const Gardener actors[2], Output *out) {
    if (!out->short_view) {
        output_print(out, "    ");
        for (int col = 0; col < g->cols; ++col) output_print(out, "%2d  ", col);
        output_print(out, "\n");
    }
    print_border(g, out);
    for (int row = 0; row < g->rows; ++row) {
        output_print(out, "%2d |", row);
        for (int col = 0; col < g->cols; ++col) {
            int cell = row * g->cols + col;
            char symbol = g->cells[cell];
            for (int id = 0; id < 2; ++id)
                if (actors[id].position == cell) symbol = (char)('1' + id);
            const char *color = symbol == '1' ? "\033[1;36m" :
                                symbol == '2' ? "\033[1;33m" :
                                symbol == '+' ? "\033[32m" : "\033[90m";
            output_control(out, color);
            output_print(out, " %c ", symbol);
            output_control(out, "\033[0m");
            output_print(out, "|");
        }
        output_print(out, "\n");
        if (!out->compact || row == g->rows - 1) print_border(g, out);
    }
    const char *states[] = {"готов", "перемещается", "обрабатывает", "ожидает", "завершил"};
    for (int id = 0; id < 2; ++id)
        output_print(out, "Садовник %d: %s.\n", id + 1, states[actors[id].state]);
}
