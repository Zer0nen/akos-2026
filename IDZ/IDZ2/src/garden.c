#include "garden.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int neighbours(const Garden *g, int cell, int next[4]) {
    int row = cell / g->cols, col = cell % g->cols, count = 0;
    if (col + 1 < g->cols) next[count++] = cell + 1;
    if (row + 1 < g->rows) next[count++] = cell + g->cols;
    if (col > 0) next[count++] = cell - 1;
    if (row > 0) next[count++] = cell - g->cols;
    return count;
}

/* До запуска потоков отмечаем достижимые из каждого старта клетки обычным BFS. */
static void mark_reachable(Garden *g, int id, int start) {
    int queue[MAX_CELLS], head = 0, tail = 0;
    queue[tail++] = start;
    g->reachable[id][start] = true;
    while (head < tail) {
        int next[4], count = neighbours(g, queue[head++], next);
        for (int i = 0; i < count; ++i) {
            if (g->cells[next[i]] != '#' && !g->reachable[id][next[i]]) {
                g->reachable[id][next[i]] = true;
                queue[tail++] = next[i];
            }
        }
    }
}

/* Сначала проверяем число целиком, затем безопасно преобразуем его в int. */
static bool read_int(FILE *file, int *result) {
    char token[64], *end;
    if (fscanf(file, "%63s", token) != 1) return false;
    int next = fgetc(file);
    if (next != EOF && !isspace((unsigned char)next)) return false;
    errno = 0;
    long value = strtol(token, &end, 10);
    if (errno == ERANGE || end == token || *end != '\0' || value < INT_MIN || value > INT_MAX)
        return false;
    *result = (int)value;
    return true;
}

int garden_load(Garden *g, Config *cfg, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("Входной файл"); return -1; }
    FILE *file = fdopen(fd, "r");
    if (!file) { perror("fdopen"); close(fd); return -1; }
    int r1, c1, r2, c2, detours;
    char route;
    memset(g, 0, sizeof *g);
    if (!read_int(file, &g->rows) || !read_int(file, &g->cols) ||
        !read_int(file, &cfg->move[0]) || !read_int(file, &cfg->move[1]) ||
        !read_int(file, &cfg->work) || fscanf(file, " %c", &route) != 1 ||
        !read_int(file, &detours) || !read_int(file, &r1) || !read_int(file, &c1) ||
        !read_int(file, &r2) || !read_int(file, &c2) || !read_int(file, &cfg->unit_ms)) goto invalid;
    if (g->rows < 1 || g->rows > MAX_SIDE || g->cols < 1 || g->cols > MAX_SIDE ||
        g->rows * g->cols < 2 || (route != 'C' && route != 'R') ||
        (detours != 0 && detours != 1) || cfg->unit_ms < 0 || cfg->unit_ms > 1000 ||
        cfg->move[0] < 1 || cfg->move[0] > 100 ||
        cfg->move[1] < 1 || cfg->move[1] > 100 || cfg->work < 1 || cfg->work > 100 ||
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
    cfg->columns = route == 'C';
    cfg->detours = detours != 0;
    if (fclose(file) != 0) { perror("Закрытие входного файла"); return -1; }
    for (int id = 0; id < 2; ++id) mark_reachable(g, id, cfg->start[id]);
    for (int cell = 0; cell < g->rows * g->cols; ++cell)
        if (g->reachable[0][cell] || g->reachable[1][cell]) ++g->reachable_count;
    return 0;
invalid:
    dprintf(STDERR_FILENO, "Некорректные параметры или карта: %s. Формат — в отчёте.\n", path);
    fclose(file);
    return -1;
}

/* В движении другой участник сохраняет исходную позицию и резервирует назначение. */
static bool free_cell(const Simulation *s, int id, int cell) {
    const Gardener *other = &s->actors[1 - id];
    return s->garden.cells[cell] != '#' && cell != other->position &&
           !(other->state == MOVING && cell == other->destination);
}

int garden_next_step(const Simulation *s, int id, int target) {
    const Garden *g = &s->garden;
    int start = s->actors[id].position;
    if (!s->cfg.detours) {
        int next = start;
        if (target % g->cols != start % g->cols)
            next += target % g->cols > start % g->cols ? 1 : -1;
        else next += target / g->cols > start / g->cols ? g->cols : -g->cols;
        return free_cell(s, id, next) ? next : -1;
    }
    int previous[MAX_CELLS], queue[MAX_CELLS], head = 0, tail = 0;
    for (int i = 0; i < g->rows * g->cols; ++i) previous[i] = -1;
    previous[start] = start;
    queue[tail++] = start;
    while (head < tail && previous[target] == -1) {
        int next[4], count = neighbours(g, queue[head++], next);
        for (int i = 0; i < count; ++i) {
            if (previous[next[i]] == -1 && free_cell(s, id, next[i])) {
                previous[next[i]] = queue[head - 1];
                queue[tail++] = next[i];
            }
        }
    }
    if (previous[target] == -1) return -1;
    int next = target;
    while (previous[next] != start) next = previous[next];
    return next;
}

/* Эту функцию вызывают под мьютексом либо после pthread_join. */
void garden_draw(Simulation *s, int fd, bool colors) {
    const Garden *g = &s->garden;
    for (int row = 0; row < g->rows; ++row) {
        for (int col = 0; col < g->cols; ++col)
            if (dprintf(fd, "+---") < 0) s->failed = true;
        if (dprintf(fd, "+\n|") < 0) s->failed = true;
        for (int col = 0; col < g->cols; ++col) {
            int cell = row * g->cols + col;
            char symbol = g->cells[cell];
            for (int id = 0; id < 2; ++id)
                if (s->actors[id].position == cell) symbol = (char)('1' + id);
            const char *color = !colors ? "" : symbol == '1' ? "\033[1;36m" :
                                symbol == '2' ? "\033[1;33m" : symbol == '+' ? "\033[32m" : "\033[0m";
            if (dprintf(fd, "%s %c %s|", color, symbol, colors ? "\033[0m" : "") < 0) s->failed = true;
        }
        if (dprintf(fd, "\n") < 0) s->failed = true;
    }
    for (int col = 0; col < g->cols; ++col)
        if (dprintf(fd, "+---") < 0) s->failed = true;
    if (dprintf(fd, "+\n") < 0) s->failed = true;
}
