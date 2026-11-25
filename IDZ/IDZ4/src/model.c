#include "garden.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Чтение карты и правила маршрутов перенесены из ИДЗ3. */
int parse_int(const char *text, int *value) {
    char *end;
    errno = 0;
    long number = strtol(text, &end, 10);
    if (errno == ERANGE || end == text || *end || number < INT_MIN || number > INT_MAX) return -1;
    *value = (int)number;
    return 0;
}
static bool read_int(FILE *file, int *value) {
    char text[64];
    if (fscanf(file, "%63s", text) != 1) return false;
    int next = fgetc(file);
    return (next == EOF || isspace((unsigned char)next)) && parse_int(text, value) == 0;
}
static int neighbours(const Config *c, int cell, int next[4]) {
    int row = cell / c->cols, col = cell % c->cols, count = 0;
    if (col + 1 < c->cols) next[count++] = cell + 1;
    if (row + 1 < c->rows) next[count++] = cell + c->cols;
    if (col > 0) next[count++] = cell - 1;
    if (row > 0) next[count++] = cell - c->cols;
    return count;
}
static bool free_cell(const State *s, const Actor *other, int cell) {
    if (s->cells[cell] == '#') return false;
    return !other || (cell != other->position &&
           (other->status != MOVING || cell != other->destination));
}

/* Очередь хранит клетки по расстоянию, first — первый шаг к каждой клетке.
   Цель -1 означает полный обход, other == NULL учитывает только препятствия. */
static int explore(const State *s, int start, int target, const Actor *other,
                   int queue[CELLS], int first[CELLS]) {
    for (int i = 0; i < s->cfg.rows * s->cfg.cols; ++i) first[i] = -1;
    int tail = 1;
    queue[0] = start;
    first[start] = start;
    for (int head = 0; head < tail && (target < 0 || first[target] < 0); ++head) {
        int cell = queue[head], next[4];
        int count = neighbours(&s->cfg, cell, next);
        for (int i = 0; i < count; ++i) {
            int n = next[i];
            if (first[n] >= 0 || !free_cell(s, other, n)) continue;
            first[n] = cell == start ? n : first[cell];
            queue[tail++] = n;
        }
    }
    return tail;
}
int load_garden(State *s, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("Входной файл"); return -1; }
    FILE *file = fdopen(fd, "r");
    if (!file) { close(fd); return -1; }
    memset(s, 0, sizeof *s);
    Config *c = &s->cfg;
    int r1, c1, r2, c2, detours;
    char route;
    if (!read_int(file, &c->rows) || !read_int(file, &c->cols) ||
        !read_int(file, &c->move[0]) || !read_int(file, &c->move[1]) || !read_int(file, &c->work) ||
        fscanf(file, " %c", &route) != 1 || !read_int(file, &detours) || !read_int(file, &r1) ||
        !read_int(file, &c1) || !read_int(file, &r2) || !read_int(file, &c2) || !read_int(file, &c->unit_ms)) goto invalid;
    if (c->rows < 1 || c->rows > SIDE || c->cols < 1 || c->cols > SIDE || c->rows * c->cols < 2 ||
        c->move[0] < 1 || c->move[0] > 100 || c->move[1] < 1 || c->move[1] > 100 ||
        c->work < 1 || c->work > 100 || c->unit_ms < 0 || c->unit_ms > 1000 ||
        (route != 'C' && route != 'R') || (detours != 0 && detours != 1) ||
        r1 < 0 || r1 >= c->rows || r2 < 0 || r2 >= c->rows ||
        c1 < 0 || c1 >= c->cols || c2 < 0 || c2 >= c->cols) goto invalid;
    for (int row = 0; row < c->rows; ++row) {
        char line[SIDE + 1];
        if (fscanf(file, "%30s", line) != 1 || (int)strlen(line) != c->cols) goto invalid;
        int separator = fgetc(file);
        if (separator != EOF && !isspace((unsigned char)separator)) goto invalid;
        for (int col = 0; col < c->cols; ++col) {
            if (line[col] != '.' && line[col] != '#') goto invalid;
            s->cells[row * c->cols + col] = line[col];
        }
    }
    char extra;
    if (fscanf(file, " %c", &extra) != EOF || ferror(file)) goto invalid;
    c->start[0] = r1 * c->cols + c1;
    c->start[1] = r2 * c->cols + c2;
    if (c->start[0] == c->start[1] || s->cells[c->start[0]] == '#' || s->cells[c->start[1]] == '#') goto invalid;
    c->columns = route == 'C'; c->detours = detours != 0;
    if (fclose(file) != 0) return -1;

    for (int id = 0; id < 2; ++id) {
        int queue[CELLS], first[CELLS];
        int count = explore(s, c->start[id], -1, NULL, queue, first);
        for (int i = 0; i < count; ++i) s->reachable[id][queue[i]] = true;
        s->actors[id].position = c->start[id];
        s->actors[id].destination = -1;
    }
    for (int cell = 0; cell < c->rows * c->cols; ++cell)
        if (s->reachable[0][cell] || s->reachable[1][cell]) ++s->reachable_count;
    return 0;
invalid:
    dprintf(STDERR_FILENO, "Некорректные параметры или карта: %s\n", path);
    fclose(file);
    return -1;
}

void make_route(const State *s, int id, int route[CELLS]) {
    const Config *c = &s->cfg;
    int order[CELLS], count = 0;
    if (id == 0 || !c->columns) {
        for (int step = 0; step < c->rows; ++step) {
            int row = id == 0 ? step : c->rows - 1 - step;
            for (int k = 0; k < c->cols; ++k)
                order[count++] = row * c->cols + ((step + id) % 2 ? c->cols - 1 - k : k);
        }
    } else {
        for (int step = 0; step < c->cols; ++step)
            for (int k = 0; k < c->rows; ++k)
                order[count++] = (step % 2 ? k : c->rows - 1 - k) * c->cols + c->cols - 1 - step;
    }
    int offset = 0;
    while (order[offset] != c->start[id]) ++offset;
    for (int i = 0; i < count; ++i) route[i] = order[(offset + i) % count];
}
int next_step(const State *s, int id, int target) {
    const Config *c = &s->cfg;
    int start = s->actors[id].position;
    const Actor *other = &s->actors[1 - id];
    if (!c->detours) {
        int next = start;
        if (target % c->cols != start % c->cols) next += target % c->cols > start % c->cols ? 1 : -1;
        else next += target / c->cols > start / c->cols ? c->cols : -c->cols;
        return free_cell(s, other, next) ? next : -1;
    }
    int queue[CELLS], first[CELLS];
    explore(s, start, target, other, queue, first);
    return first[target];
}

/* Ищем ближайшую клетку, из которой садовник больше не перекрывает путь другому. */
int find_yield_cell(const State *s, int id, int other_target) {
    State trial = *s;
    int queue[CELLS], first[CELLS];
    int count = explore(s, s->actors[id].position, -1, &s->actors[1 - id], queue, first);
    for (int i = 1; i < count; ++i) {
        trial.actors[id].position = queue[i];
        if (next_step(&trial, 1 - id, other_target) >= 0) return queue[i];
    }
    return -1;
}

int draw_garden(int fd, int rows, int cols, const char *map, const int positions[2]) {
    char line[2 * SIDE + 1];
    line[2 * cols] = '\0';
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            int cell = row * cols + col;
            line[2 * col] = cell == positions[0] ? '1' : cell == positions[1] ? '2' : map[cell];
            line[2 * col + 1] = ' ';
        }
        line[2 * cols - 1] = '\n';
        if (dprintf(fd, "%s", line) < 0) return -1;
    }
    return 0;
}

const char *end_reason(int code) {
    if (code == 0) return "все достижимые клетки обработаны";
    if (code == 2) return "невозможно продолжить обход";
    if (code == 130 || code == 143) return "сеанс прерван";
    return "ошибка";
}

int describe_remaining(int fd, int cols, const char *map) {
    for (int i = 0; map[i]; ++i) {
        if (map[i] != 'r' && map[i] != 'u') continue;
        if (dprintf(fd, "Необработанная клетка (%d,%d): %s.\n", i / cols, i % cols,
                    map[i] == 'r' ? "обход не завершён" : "недостижима из обоих стартов") < 0) return -1;
    }
    return 0;
}
