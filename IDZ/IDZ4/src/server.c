#include "garden.h"
#include "net.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PEERS 24
/* Даже серия пропусков всей карты у обоих садовников помещается в буфер. */
#define OUTPUT_CAP (256 * 1024)

typedef enum { HELLO, WORKER, VIEWER } Role;
typedef struct {
    int fd, id;
    Role role;
    bool failed, closing;
    Reader input;
    char output[OUTPUT_CAP];
    size_t pending;
} Peer;

typedef struct {
    State garden;
    Peer peers[PEERS];
    int route[2][CELLS], cursor[2], ticket[2];
    int slot[2], worked, log_fd;
    int yielding, yield_cell;
    bool watch, started, dirty, log_error;
} Server;

/* Буфер имеет предел: медленный клиент не задерживает остальных. */
static void enqueue(Peer *peer, const char *line) {
    if (peer->failed || peer->closing) return;
    size_t size = strlen(line);
    if (size > sizeof peer->output - peer->pending) {
        peer->failed = true;
        return;
    }
    memcpy(peer->output + peer->pending, line, size);
    peer->pending += size;
}

static void flush(Peer *peer) {
    if (!peer->pending) return;
    ssize_t n = send(peer->fd, peer->output, peer->pending, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n > 0) {
        peer->pending -= (size_t)n;
        memmove(peer->output, peer->output + n, peer->pending);
    } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        peer->failed = true;
    }
}

static void event(Server *s, int id, const char *format, ...) {
    char text[200], line[LINE_CAP];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    snprintf(line, sizeof line, "INFO Садовник %d: %s.\n", id + 1, text);
    for (int i = 0; i < PEERS; ++i) {
        Peer *p = &s->peers[i];
        if (p->fd >= 0 && (p->role == VIEWER || (p->role == WORKER && p->id == id)))
            enqueue(p, line);
    }
    if (s->log_fd >= 0 && dprintf(s->log_fd, "%s", line + 5) < 0) s->log_error = true;
    s->dirty = true;
}

static void publish_view(Server *s) {
    const State *g = &s->garden;
    const Actor *a = g->actors;
    int rows = g->cfg.rows, cols = g->cfg.cols;
    int first = s->slot[0] < 0 && a[0].status != FINISHED ? OFFLINE : a[0].status;
    int second = s->slot[1] < 0 && a[1].status != FINISHED ? OFFLINE : a[1].status;
    char line[LINE_CAP];
    snprintf(line, sizeof line, "VIEW %d %d %d %d %d %d %d %d %.*s\n",
             rows, cols, s->worked, g->reachable_count, a[0].position, first,
             a[1].position, second, rows * cols, g->cells);
    for (int i = 0; i < PEERS; ++i)
        if (s->peers[i].fd >= 0 && s->peers[i].role == VIEWER) enqueue(&s->peers[i], line);
    s->dirty = false;
}

static void drop(Server *s, int index) {
    Peer *peer = &s->peers[index];
    close(peer->fd);
    peer->fd = -1;
    if (peer->role != WORKER) return;
    int id = peer->id;
    Actor *a = &s->garden.actors[id];
    s->slot[id] = -1;
    s->yielding = -1;
    /* Сохраняем только подтверждённую позицию и карту. Действие начнётся заново. */
    a->destination = -1;
    if (a->status != FINISHED) {
        a->status = READY;
        event(s, id, "отключился, прогресс сохранён");
    }
}

static void finish_actor(Actor *a) {
    a->status = FINISHED;
    a->position = a->destination = -1;
}

static void start_action(Server *s, int id, int next) {
    State *g = &s->garden;
    Actor *a = &g->actors[id];
    int cols = g->cfg.cols;
    bool work = a->position == next;
    a->status = work ? PROCESSING : MOVING;
    if (work) event(s, id, "начал обработку (%d,%d)", next / cols, next % cols);
    else {
        a->destination = next;
        event(s, id, "начал переход (%d,%d) -> (%d,%d)",
              a->position / cols, a->position % cols, next / cols, next % cols);
    }
    s->ticket[id] = s->ticket[id] == INT_MAX ? 1 : s->ticket[id] + 1;
    int ms = (work ? g->cfg.work : g->cfg.move[id]) * g->cfg.unit_ms;
    char line[64];
    snprintf(line, sizeof line, "ACT %d %d\n", s->ticket[id], ms);
    enqueue(&s->peers[s->slot[id]], line);
}

static void advance(Server *s, int id) {
    State *g = &s->garden;
    Actor *a = &g->actors[id];
    int cols = g->cfg.cols, count = g->cfg.rows * cols;
    if (s->slot[id] < 0 || (a->status != READY && a->status != WAITING)) return;
    int *cursor = &s->cursor[id], previous = *cursor;
    while (*cursor < count) {
        int cell = s->route[id][*cursor];
        if (g->cells[cell] == '#')
            event(s, id, "пропустил (%d,%d): препятствие", cell / cols, cell % cols);
        else if (g->cells[cell] != '+' && g->reachable[id][cell])
            break;
        ++*cursor;
    }
    if (*cursor == count) {
        finish_actor(a);
        event(s, id, "закончил маршрут и освободил клетку");
        return;
    }
    int target = s->route[id][*cursor];
    int next = a->position == target ? target : next_step(g, id, target);
    if (next < 0) {
        if (a->status != WAITING || previous != *cursor)
            event(s, id, "ожидает путь к (%d,%d)", target / cols, target % cols);
        a->status = WAITING;
        return;
    }
    start_action(s, id, next);
}

static void complete(Server *s, int id) {
    State *g = &s->garden;
    Actor *a = &g->actors[id];
    int cols = g->cfg.cols;
    if (a->status == PROCESSING) {
        g->cells[a->position] = '+';
        ++a->processed;
        ++s->worked;
        ++s->cursor[id];
        if (s->yielding == 1 - id) s->yielding = -1;
        event(s, id, "закончил обработку (%d,%d)", a->position / cols, a->position % cols);
    } else {
        a->position = a->destination;
        a->destination = -1;
        event(s, id, "закончил переход в (%d,%d)", a->position / cols, a->position % cols);
    }
    a->status = READY;
}

static void reject(Peer *p, const char *reason) {
    char line[128];
    snprintf(line, sizeof line, "INFO %s\nEND 1\n", reason);
    enqueue(p, line);
    p->closing = true;
}

static void on_line(Server *s, int index, const char *line) {
    Peer *p = &s->peers[index];
    if (p->role == HELLO) {
        if (!strcmp(line, "WATCH")) {
            if (!s->watch) { reject(p, "Наблюдение выключено в режиме basic."); return; }
            p->role = VIEWER;
            s->dirty = true;
            return;
        }
        if (strcmp(line, "WORK 1") && strcmp(line, "WORK 2")) {
            reject(p, "Неизвестная роль клиента.");
            return;
        }
        int id = line[5] - '1';
        if (s->slot[id] >= 0) { reject(p, "Этот садовник уже подключён."); return; }
        p->role = WORKER;
        p->id = id;
        s->slot[id] = index;
        if (s->garden.actors[id].status == FINISHED) event(s, id, "маршрут уже закончен, ждёт итога");
        else if (s->started) event(s, id, "переподключился и продолжает с сохранённой позиции");
        else event(s, id, "подключился, работа начнётся после подключения обоих");
        return;
    }
    int ticket;
    if (p->role == WORKER && !strncmp(line, "DONE ", 5) && parse_int(line + 5, &ticket) == 0 &&
        ticket == s->ticket[p->id]) {
        Status state = s->garden.actors[p->id].status;
        if (state == MOVING || state == PROCESSING) { complete(s, p->id); return; }
    }
    p->failed = true;
}

static void accept_peer(Server *s, int listener) {
    int fd = accept(listener, NULL, NULL);
    if (fd < 0) return;
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
    for (int i = 0; i < PEERS; ++i) if (s->peers[i].fd < 0) {
        s->peers[i] = (Peer){.fd = fd};
        return;
    }
    close(fd);
}

/* Уступающий ждёт в стороне, пока второй не закончит обработку своей цели. */
static int continue_yield(Server *s) {
    int id = s->yielding;
    const Actor *a = &s->garden.actors[id];
    if (a->status == MOVING) return -1;
    if (a->position == s->yield_cell) {
        advance(s, 1 - id);
    } else {
        int step = next_step(&s->garden, id, s->yield_cell);
        if (step < 0) return 2;
        start_action(s, id, step);
    }
    return -1;
}

/* Возвращает -1, пока есть действие или ожидается переподключение. */
static int schedule(Server *s) {
    if (!s->started) {
        if (s->slot[0] < 0 || s->slot[1] < 0) return -1;
        s->started = true;
    }
    if (s->worked == s->garden.reachable_count) return 0;
    if (s->yielding >= 0) return continue_yield(s);
    for (int id = 0; id < 2; ++id) advance(s, id);
    /* Если второй освободил клетку, первый может выйти из ожидания сразу. */
    if (s->garden.actors[0].status == WAITING && s->garden.actors[1].status == FINISHED) advance(s, 0);
    for (int id = 0; id < 2; ++id) {
        Status status = s->garden.actors[id].status;
        if (status != FINISHED && (s->slot[id] < 0 || status != WAITING)) return -1;
    }
    const State *g = &s->garden;
    if (g->cfg.detours && g->actors[0].status == WAITING && g->actors[1].status == WAITING) {
        for (int id = 0; id < 2; ++id) {
            int other = 1 - id;
            int cell = find_yield_cell(g, id, s->route[other][s->cursor[other]]);
            if (cell < 0) continue;
            s->yielding = id;
            s->yield_cell = cell;
            event(s, id, "уступает дорогу садовнику %d, отходит в (%d,%d)",
                  other + 1, cell / g->cfg.cols, cell % g->cfg.cols);
            return continue_yield(s);
        }
    }
    return 2;
}

static int serve(Server *s, int listener) {
    while (!interrupted) {
        struct pollfd fds[PEERS + 1] = {{.fd = listener, .events = POLLIN}};
        for (int i = 0; i < PEERS; ++i) {
            Peer *p = &s->peers[i];
            fds[i + 1] = (struct pollfd){.fd = p->fd,
                .events = (p->closing ? 0 : POLLIN) | (p->pending ? POLLOUT : 0)};
        }
        if (poll(fds, PEERS + 1, 100) < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        if (interrupted) break;
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) return 1;
        for (int i = 0; i < PEERS; ++i) {
            Peer *p = &s->peers[i];
            if (p->fd < 0) continue;
            short events = fds[i + 1].revents;
            if (!p->closing && (events & (POLLIN | POLLHUP))) {
                char line[LINE_CAP];
                int n = 0;
                while (!p->failed && !p->closing && (n = read_line(p->fd, &p->input, line)) > 0)
                    on_line(s, i, line);
                if (n < 0) p->failed = true;
            }
            if (events & POLLOUT) flush(p);
            if (events & (POLLERR | POLLHUP | POLLNVAL)) p->failed = true;
            if (p->failed || (p->closing && !p->pending)) drop(s, i);
        }
        if (fds[0].revents & POLLIN) accept_peer(s, listener);
        int result = schedule(s);
        if (s->log_error) return 1;
        if (result >= 0) return result;
        if (s->dirty && s->watch) publish_view(s);
    }
    return 128 + interrupted;
}

static int finish(Server *s, int result) {
    State *g = &s->garden;
    int cols = g->cfg.cols, count = g->cfg.rows * cols;
    for (int id = 0; id < 2; ++id) finish_actor(&g->actors[id]);
    if (s->watch) publish_view(s);
    char left[CELLS + 1], line[LINE_CAP];
    for (int i = 0; i < count; ++i) {
        left[i] = g->cells[i] != '.' ? '-' : g->reachable[0][i] || g->reachable[1][i] ? 'r' : 'u';
    }
    left[count] = '\0';
    /* Диагностика всей карты помещается в одном сообщении даже при большой карте. */
    snprintf(line, sizeof line, "LEFT %d %d %s\n", g->cfg.rows, cols, left);
    for (int i = 0; i < PEERS; ++i) if (s->peers[i].fd >= 0 && s->peers[i].role == VIEWER)
        enqueue(&s->peers[i], line);
    for (int id = 0; id < 2; ++id) if (s->slot[id] >= 0) {
        char own[CELLS + 1];
        for (int i = 0; i < count; ++i) own[i] = left[i] == 'r' && !g->reachable[id][i] ? '-' : left[i];
        own[count] = '\0';
        snprintf(line, sizeof line, "LEFT %d %d %s\n", g->cfg.rows, cols, own);
        enqueue(&s->peers[s->slot[id]], line);
    }
    if (s->log_fd >= 0) {
        const int positions[] = {-1, -1};
        if (draw_garden(s->log_fd, g->cfg.rows, cols, g->cells, positions) < 0 ||
            describe_remaining(s->log_fd, cols, left) < 0 ||
            dprintf(s->log_fd, "Итог: обработано %d из %d достижимых клеток. По садовникам: %d и %d.\n",
                s->worked, g->reachable_count, g->actors[0].processed, g->actors[1].processed) < 0 ||
            dprintf(s->log_fd, "Завершение: %s.\n", end_reason(result)) < 0) s->log_error = true;
    }
    if (s->log_error) result = 1;
    snprintf(line, sizeof line, "END %d\n", result);
    for (int i = 0; i < PEERS; ++i) if (s->peers[i].fd >= 0) enqueue(&s->peers[i], line);

    /* После END закрываем запись и дочитываем вход, чтобы не оборвать отправку через RST. */
    long long until = monotonic_ms() + 1000;
    bool sent_end[PEERS] = {0};
    for (;;) {
        struct pollfd fds[PEERS];
        bool pending = false;
        for (int i = 0; i < PEERS; ++i) {
            Peer *p = &s->peers[i];
            if (p->fd >= 0 && !p->failed && !p->pending && !sent_end[i]) {
                if (shutdown(p->fd, SHUT_WR) < 0) p->failed = true;
                sent_end[i] = true;
            }
            int fd = p->failed ? -1 : p->fd;
            fds[i] = (struct pollfd){.fd = fd, .events = POLLIN | (p->pending ? POLLOUT : 0)};
            pending |= fd >= 0;
        }
        long long left = until - monotonic_ms();
        if (!pending || left <= 0 || left > 1000) break;
        int ready = poll(fds, PEERS, (int)left);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        for (int i = 0; i < PEERS; ++i) {
            Peer *p = &s->peers[i];
            short events = fds[i].revents;
            if (events & POLLOUT) flush(p);
            if (events & POLLIN) {
                char ignored[LINE_CAP];
                ssize_t n = recv(p->fd, ignored, sizeof ignored, MSG_DONTWAIT);
                if (n == 0) p->failed = true;
            }
            if (events & (POLLERR | POLLHUP | POLLNVAL)) p->failed = true;
        }
    }
    for (int i = 0; i < PEERS; ++i) if (s->peers[i].fd >= 0) close(s->peers[i].fd);
    return result;
}

int main(int argc, char **argv) {
    static Server s = {.slot = {-1, -1}, .log_fd = -1, .yielding = -1};
    int port;
    if (argc < 4 || argc > 5 || port_number(argv[2], 0, &port) < 0 ||
        (strcmp(argv[3], "basic") && strcmp(argv[3], "watch"))) {
        fprintf(stderr, "Запуск: garden4_server IP ПОРТ basic|watch [карта]\n");
        return 1;
    }
    for (int i = 0; i < PEERS; ++i) s.peers[i].fd = -1;
    s.watch = !strcmp(argv[3], "watch");
    if (load_garden(&s.garden, argc == 5 ? argv[4] : "data/demo.txt") < 0) return 1;
    for (int id = 0; id < 2; ++id) make_route(&s.garden, id, s.route[id]);
    if (signals_init() < 0) { perror("Сигналы"); return 1; }
    int listener = tcp_open(argv[1], port, 1);
    if (listener < 0) { perror("Сервер TCP"); return 1; }
    struct sockaddr_in address;
    socklen_t length = sizeof address;
    if (getsockname(listener, (struct sockaddr *)&address, &length) < 0) {
        perror("Порт сервера");
        close(listener);
        return 1;
    }
    if (s.watch) {
        s.log_fd = open("garden.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (s.log_fd < 0) { perror("Журнал"); close(listener); return 1; }
    }
    int result = dprintf(STDOUT_FILENO, "READY %s %u (%s)\n", argv[1], (unsigned)ntohs(address.sin_port), argv[3]) < 0
                 ? 1 : serve(&s, listener);
    close(listener);
    result = finish(&s, result);
    if (s.log_fd >= 0) {
        close(s.log_fd);
        puts("Журнал: garden.log");
    }
    printf("Завершение сеанса: %s.\n", end_reason(result));
    return result;
}
