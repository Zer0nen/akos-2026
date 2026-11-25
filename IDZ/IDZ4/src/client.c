#include "garden.h"
#include "net.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    int rows, cols, done, total, position[2], status[2];
    char cells[CELLS + 1], left[CELLS + 1];
    int left_cols;
} View;

static bool next_number(char **rest, int *value) {
    char *word = strtok_r(NULL, " ", rest);
    return word && parse_int(word, value) == 0;
}

static bool read_map(char **rest, int rows, int cols, const char *symbols, char cells[CELLS + 1]) {
    char *map = strtok_r(NULL, " ", rest);
    if (rows < 1 || rows > SIDE || cols < 1 || cols > SIDE ||
        !map || strspn(map, symbols) != (size_t)(rows * cols) ||
        strlen(map) != (size_t)(rows * cols) || strtok_r(NULL, " ", rest)) return false;
    strcpy(cells, map);
    return true;
}

static bool read_view(View *v, char **rest) {
    if (!next_number(rest, &v->rows) || !next_number(rest, &v->cols) ||
        !next_number(rest, &v->done) || !next_number(rest, &v->total) ||
        !next_number(rest, &v->position[0]) || !next_number(rest, &v->status[0]) ||
        !next_number(rest, &v->position[1]) || !next_number(rest, &v->status[1]) ||
        !read_map(rest, v->rows, v->cols, ".#+", v->cells)) return false;
    int count = v->rows * v->cols;
    for (int id = 0; id < 2; ++id)
        if (v->position[id] < -1 || v->position[id] >= count ||
            v->status[id] < READY || v->status[id] > OFFLINE) return false;
    return v->done >= 0 && v->done <= v->total && v->total <= count;
}

static int show(const View *v) {
    static const char *status[] = {"готов", "в пути", "обрабатывает", "ожидает", "закончил", "нет связи"};
    if (dprintf(1, "Обработано %d из %d.  1: %s  |  2: %s\n", v->done, v->total,
                status[v->status[0]], status[v->status[1]]) < 0) return -1;
    return draw_garden(1, v->rows, v->cols, v->cells, v->position);
}

static int run(int fd, bool observer) {
    Reader input = {0};
    View view = {0};
    int ticket = 0;
    long long due = 0;
    while (!interrupted) {
        long long now = monotonic_ms();
        if (now < 0) break;
        long long left = ticket ? due - now : 100;
        int timeout = left <= 0 ? 0 : left > 100 ? 100 : (int)left;
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (poll(&p, 1, timeout) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (interrupted) break;
        if (p.revents) {
            if (p.revents & POLLNVAL) break;
            char line[LINE_CAP];
            int n;
            while ((n = read_line(fd, &input, line)) > 0) {
                if (!strncmp(line, "INFO ", 5)) {
                    if (dprintf(1, "%s\n", line + 5) < 0) goto done;
                    continue;
                }
                char *rest, *type = strtok_r(line, " ", &rest);
                if (!type) goto done;
                if (!strcmp(type, "END")) {
                    int code;
                    if (next_number(&rest, &code) && !strtok_r(NULL, " ", &rest) &&
                        (code == 0 || code == 1 || code == 2 || code == 130 || code == 143)) {
                        if (view.left_cols && describe_remaining(1, view.left_cols, view.left) < 0) return 1;
                        printf("Сервер завершил сеанс: %s (код %d).\n", end_reason(code), code);
                        return code;
                    }
                    goto done;
                }
                if (observer && !strcmp(type, "VIEW")) {
                    if (!read_view(&view, &rest) || show(&view) < 0) goto done;
                } else if (!strcmp(type, "LEFT")) {
                    int rows, cols;
                    if (!next_number(&rest, &rows) || !next_number(&rest, &cols) ||
                        !read_map(&rest, rows, cols, "-ru", view.left)) goto done;
                    view.left_cols = cols;
                } else if (!observer && !ticket && !strcmp(type, "ACT")) {
                    int action, ms;
                    now = monotonic_ms();
                    if (now < 0 || !next_number(&rest, &action) || !next_number(&rest, &ms) ||
                        action < 1 || ms < 0 || ms > 100000 || strtok_r(NULL, " ", &rest)) goto done;
                    ticket = action;
                    due = now + ms;
                } else goto done;
                if (interrupted) goto done;
            }
            if (n < 0 || (p.revents & (POLLERR | POLLHUP))) break;
        }
        if (!interrupted && ticket && monotonic_ms() >= due) {
            char message[32];
            snprintf(message, sizeof message, "DONE %d\n", ticket);
            if (send_text(fd, message) < 0) break;
            ticket = 0;
        }
    }
done:
    if (interrupted) return 128 + interrupted;
    fprintf(stderr, "Соединение завершилось без сообщения END.\n");
    return 1;
}

int main(int argc, char **argv) {
    int port;
#ifdef OBSERVER
    bool observer = true;
    if (argc != 3 || port_number(argv[2], 1, &port) < 0) {
        fprintf(stderr, "Запуск: garden4_view IP ПОРТ\n");
        return 1;
    }
    const char *hello = "WATCH\n";
#else
    bool observer = false;
    if (argc != 4 || port_number(argv[2], 1, &port) < 0 || (strcmp(argv[3], "1") && strcmp(argv[3], "2"))) {
        fprintf(stderr, "Запуск: garden4_worker IP ПОРТ 1|2\n");
        return 1;
    }
    const char *hello = argv[3][0] == '1' ? "WORK 1\n" : "WORK 2\n";
#endif
    if (signals_init() < 0) { perror("Сигналы"); return 1; }
    int fd = tcp_open(argv[1], port, 0);
    if (fd < 0) { perror("Подключение TCP"); return interrupted ? 128 + interrupted : 1; }
    int result = 1;
    if (send_text(fd, hello) == 0) result = run(fd, observer);
    else perror("Регистрация клиента");
    close(fd);
    return result;
}
