#include "garden.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

volatile sig_atomic_t interrupted;
static void on_signal(int number) { interrupted = number; }
int signals_init(void) {
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    signal(SIGPIPE, SIG_IGN);
    return sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL) ? -1 : 0;
}

/* kill(pid, 0) видит и zombie. В Linux отличаем его по состоянию в /proc. */
bool process_alive(pid_t pid) {
    if (pid <= 0) return false;
    char path[64], data[512];
    snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        ssize_t size = read(fd, data, sizeof data - 1);
        close(fd);
        if (size > 0) {
            data[size] = '\0';
            char *end = strrchr(data, ')');  /* Имя процесса само может содержать скобки. */
            if (end && end[1] == ' ' && end[2])
                return end[2] != 'Z' && end[2] != 'X' && end[2] != 'x';
        }
    }
    /* Если /proc недоступен, не принимаем ошибку доступа за смерть процесса. */
    return kill(pid, 0) == 0 || errno != ESRCH;
}

/* Проверяется перед публикацией состояния в отладочной сборке. */
bool state_valid(const State *s) {
    if (s->cfg.rows < 1 || s->cfg.rows > SIDE || s->cfg.cols < 1 || s->cfg.cols > SIDE) return false;
    int count = s->cfg.rows * s->cfg.cols, processed = 0, finished = 0;
    for (int cell = 0; cell < count; ++cell) {
        if (s->cells[cell] != '.' && s->cells[cell] != '#' && s->cells[cell] != '+') return false;
        processed += s->cells[cell] == '+';
    }
    for (int id = 0; id < 2; ++id) {
        const Actor *a = &s->actors[id], *other = &s->actors[1 - id];
        if (a->processed < 0 || a->status < READY || a->status > FINISHED) return false;
        if (a->status == FINISHED) {
            if (a->position != -1 || a->destination != -1) return false;
            ++finished;
            continue;
        }
        if (a->position < 0 || a->position >= count || s->cells[a->position] == '#' ||
            a->position == other->position) return false;
        if (a->status == PROCESSING && s->cells[a->position] != '.') return false;
        if (a->status != MOVING) {
            if (a->destination != -1) return false;
            continue;
        }
        int next = a->destination, cols = s->cfg.cols;
        if (next < 0 || next >= count || s->cells[next] == '#' || next == other->position ||
            (other->status == MOVING && next == other->destination) ||
            abs(next / cols - a->position / cols) + abs(next % cols - a->position % cols) != 1) return false;
    }
    return finished == s->finished && processed == s->actors[0].processed + s->actors[1].processed &&
           processed <= s->reachable_count;
}

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
static void reachable(State *s, int id) {
    int queue[CELLS], head = 0, tail = 0;
    queue[tail++] = s->cfg.start[id];
    s->reachable[id][queue[0]] = true;
    while (head < tail) {
        int next[4], count = neighbours(&s->cfg, queue[head++], next);
        for (int i = 0; i < count; ++i)
            if (s->cells[next[i]] != '#' && !s->reachable[id][next[i]]) {
                s->reachable[id][next[i]] = true;
                queue[tail++] = next[i];
            }
    }
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
    s->magic = MAGIC; s->owner = getpid();
    for (int id = 0; id < 2; ++id) {
        reachable(s, id);
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
static bool free_cell(const State *s, int id, int cell) {
    const Actor *other = &s->actors[1 - id];
    return s->cells[cell] != '#' && cell != other->position &&
           !(other->status == MOVING && cell == other->destination);
}
int next_step(const State *s, int id, int target) {
    const Config *c = &s->cfg;
    int start = s->actors[id].position;
    if (!c->detours) {
        int next = start;
        if (target % c->cols != start % c->cols) next += target % c->cols > start % c->cols ? 1 : -1;
        else next += target / c->cols > start / c->cols ? c->cols : -c->cols;
        return free_cell(s, id, next) ? next : -1;
    }
    int previous[CELLS], queue[CELLS], head = 0, tail = 0;
    for (int i = 0; i < c->rows * c->cols; ++i) previous[i] = -1;
    previous[start] = start; queue[tail++] = start;
    while (head < tail && previous[target] == -1) {
        int cell = queue[head++], next[4], count = neighbours(c, cell, next);
        for (int i = 0; i < count; ++i)
            if (previous[next[i]] == -1 && free_cell(s, id, next[i])) {
                previous[next[i]] = cell; queue[tail++] = next[i];
            }
    }
    if (previous[target] == -1) return -1;
    int next = target;
    while (previous[next] != start) next = previous[next];
    return next;
}

void event(Ipc *ipc, int id, const char *format, ...) {
    char text[220];
    va_list args;
    va_start(args, format); vsnprintf(text, sizeof text, format, args); va_end(args);
    snprintf(ipc->state.events[id], sizeof ipc->state.events[id], "Садовник %d: %s.", id + 1, text);
    if (dprintf(ipc->log_fd, "[pid=%ld] %s\n", (long)getpid(), ipc->state.events[id]) < 0)
        ipc->state.failed = ipc->state.stopped = true;
    if (!isatty(STDOUT_FILENO) && dprintf(STDOUT_FILENO, "[pid=%ld] %s\n", (long)getpid(), ipc->state.events[id]) < 0)
        ipc->state.failed = ipc->state.stopped = true;
}
int draw(const State *s, int fd) {
    for (int row = 0; row < s->cfg.rows; ++row) {
        for (int col = 0; col < s->cfg.cols; ++col) if (dprintf(fd, "+---") < 0) return -1;
        if (dprintf(fd, "+\n|") < 0) return -1;
        for (int col = 0; col < s->cfg.cols; ++col) {
            int cell = row * s->cfg.cols + col;
            char symbol = s->cells[cell];
            for (int id = 0; id < 2; ++id)
                if (s->actors[id].position == cell) symbol = (char)('1' + id);
            if (dprintf(fd, " %c |", symbol) < 0) return -1;
        }
        if (dprintf(fd, "\n") < 0) return -1;
    }
    for (int col = 0; col < s->cfg.cols; ++col) if (dprintf(fd, "+---") < 0) return -1;
    return dprintf(fd, "+\n") < 0 ? -1 : 0;
}
int summary(const State *s, int fd) {
    if (dprintf(fd, "Завершение: %s.\n", s->result >= 128 ? "прервано пользователем" :
            s->failed ? "ошибка" : s->result == 2 ? "невозможно продолжить обход" : "сад обработан") < 0) return -1;
    if (draw(s, fd) < 0) return -1;
    for (int cell = 0; cell < s->cfg.rows * s->cfg.cols; ++cell)
        if (s->cells[cell] == '.' && dprintf(fd, "Необработанная клетка (%d,%d): %s.\n", cell / s->cfg.cols,
            cell % s->cfg.cols, s->reachable[0][cell] || s->reachable[1][cell] ?
            "обход не завершён" : "недостижима из обоих стартов") < 0) return -1;
    return dprintf(fd, "Итог: обработано %d из %d достижимых клеток. По садовникам: %d и %d.\n",
            s->actors[0].processed + s->actors[1].processed, s->reachable_count,
            s->actors[0].processed, s->actors[1].processed) < 0 ? -1 : 0;
}
