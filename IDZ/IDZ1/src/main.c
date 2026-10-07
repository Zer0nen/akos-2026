#include "garden.h"
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* Обработчик сигнала только просит основной цикл завершить моделирование. */
static volatile sig_atomic_t stopped = 0;
static void stop_simulation(int signal_number) { stopped = signal_number; }

/* Управление экраном и цвета применяются только к настоящему терминалу. */
void output_control(Output *out, const char *sequence) {
    if (out->terminal && dprintf(STDOUT_FILENO, "%s", sequence) < 0) out->failed = true;
}

/* В журнал попадает обычный текст, без управляющих последовательностей. */
void output_print(Output *out, const char *format, ...) {
    va_list args, log_args;
    va_start(args, format);
    va_copy(log_args, args);
    output_control(out, "\033[K");
    if (vdprintf(STDOUT_FILENO, format, args) < 0) out->failed = true;
    if (vdprintf(out->log_fd, format, log_args) < 0) out->failed = true;
    va_end(log_args);
    va_end(args);
}

/* В анимации события показываются под картой; журнал сохраняет их полностью. */
void output_event(Output *out, const char *format, ...) {
    va_list args, copy;
    va_start(args, format);
    va_copy(copy, args);
    if (out->animate) {
        if (out->event_count == 4) {
            memmove(out->events, out->events + 1, 3 * sizeof out->events[0]);
            --out->event_count;
        }
        vsnprintf(out->events[out->event_count++], sizeof out->events[0], format, copy);
    } else if (vdprintf(STDOUT_FILENO, format, copy) < 0) out->failed = true;
    if (vdprintf(out->log_fd, format, args) < 0) out->failed = true;
    va_end(copy);
    va_end(args);
}

static void draw_frame(const Garden *g, const Gardener actors[2], int tick, Output *out) {
    struct winsize size = {0};
    if (out->terminal) ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
    int height = size.ws_row ? size.ws_row : 24;
    out->compact = out->terminal && height < 2 * g->rows + 12;
    out->short_view = out->terminal && height < g->rows + 8;
    output_control(out, "\033[H\033[1m");
    output_print(out, "САДОВНИКИ | Такт %d | Обработано %d/%d\n",
                 tick, actors[0].processed + actors[1].processed, g->reachable_count);
    output_control(out, "\033[0m");
    if (!out->short_view) {
        output_control(out, "\033[36m");
        output_print(out, "1 — первый");
        output_control(out, "\033[33m");
        output_print(out, "  2 — второй");
        output_control(out, "\033[0m");
        output_print(out, "  . — свободно  + — обработано  # — препятствие\n");
    }
    garden_print(g, actors, out);
    if (out->terminal) {
        int lines = out->compact ? g->rows + 7 : 2 * g->rows + 6;
        if (out->short_view) lines -= 2;
        int available = height - lines - 1;
        if (available > 0) {
            if (dprintf(STDOUT_FILENO, "\033[KСобытия текущего такта (полная история — garden.log):\n") < 0)
                out->failed = true;
            int count = out->event_count < available - 1 ? out->event_count : available - 1;
            for (int i = out->event_count - count; i < out->event_count; ++i)
                if (dprintf(STDOUT_FILENO, "\033[K%s", out->events[i]) < 0) out->failed = true;
        }
        output_control(out, "\033[J");
    }
}

int main(int argc, char **argv) {
    if (argc > 2) {
        dprintf(STDERR_FILENO, "Запуск: gardeners [файл данных]\n");
        return 1;
    }
    const char *input = argc > 1 ? argv[1] : "data/demo.txt";
    Garden garden;
    Config cfg;
    if (garden_load(&garden, &cfg, input) < 0) return 1;
    Output out = {.log_fd = open("garden.log", O_WRONLY | O_CREAT | O_TRUNC, 0644),
                  .terminal = isatty(STDOUT_FILENO)};
    if (out.log_fd < 0) { perror("Открытие журнала"); return 1; }
    struct sigaction action = {0};
    action.sa_handler = stop_simulation;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0) {
        perror("Установка обработчика сигнала"); close(out.log_fd); return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    Gardener actors[2];
    for (int id = 0; id < 2; ++id) gardener_init(&actors[id], &garden, &cfg, id);

    output_print(&out, "Сад %dx%d: доступно %d клеток, достижимо %d.\n",
                 garden.rows, garden.cols, garden.open_count, garden.reachable_count);
    output_print(&out, "Маршрут второго: %s; обходы: %s. Время перехода: %d/%d; обработки: %d.\n",
                 cfg.route2_columns ? "столбцы" : "строки", cfg.detours ? "да" : "нет",
                 cfg.move_ticks[0], cfg.move_ticks[1], cfg.work_ticks);
    output_print(&out, "# — препятствие, . — не обработано, + — обработано, 1/2 — садовники.\n");
    int tick = 0, result = 0;
    /* Отдельный экран терминала обновляется на месте и не засоряет историю. */
    output_control(&out, "\033[?1049h\033[2J\033[H\033[?25l");
    out.animate = out.terminal;
    while (!stopped && !out.failed) {
        out.event_count = 0;
        if (out.terminal) {
            if (dprintf(out.log_fd, "\nТакт %d\n", tick) < 0) out.failed = true;
        } else output_print(&out, "\nТакт %d\n", tick);
        /* Сначала завершаются действия обоих участников, затем начинаются новые. */
        for (int id = 0; id < 2; ++id) gardener_complete(&actors[id], &garden, tick, &out);
        int processed = actors[0].processed + actors[1].processed;
        if (processed == garden.reachable_count) break;
        bool changed = false, pending = false;
        for (int turn = 0; turn < 2; ++turn) {
            int id = (tick + turn) % 2;      /* Участники по очереди получают приоритет */
            bool step = gardener_step(&actors[id], &garden, actors, &cfg, tick, &out);
            changed = changed || step;
        }
        for (int id = 0; id < 2; ++id)
            if (actors[id].state == MOVING || actors[id].state == PROCESSING) pending = true;
        assert(actors[0].position < 0 || actors[1].position < 0 ||
               actors[0].position != actors[1].position);
        draw_frame(&garden, actors, tick, &out);
        if (!pending && !changed) { result = 2; break; }
        struct timespec delay = {cfg.delay_ms / 1000, (cfg.delay_ms % 1000) * 1000000L};
        if (cfg.delay_ms) nanosleep(&delay, NULL);
        ++tick;
    }
    output_control(&out, "\033[0m\033[?25h\033[?1049l");
    out.animate = false;
    if (stopped) result = 128 + stopped;
    output_print(&out, "\nЗавершение: %s.\n", stopped ? "прервано пользователем" :
                 result == 2 ? "невозможно продолжить обход" : out.failed ? "ошибка вывода" : "сад обработан");
    for (int id = 0; id < 2; ++id) gardener_finish(&actors[id], tick, &out);
    garden_print(&garden, actors, &out);
    for (int i = 0; i < garden.rows * garden.cols; ++i) {
        if (garden.cells[i] == '.')
            output_print(&out, "Необработанная клетка (%d,%d): %s.\n", i / garden.cols, i % garden.cols,
                         garden.reachable[0][i] || garden.reachable[1][i] ?
                         "обход не завершён" : "недостижима из обоих стартов");
    }
    output_print(&out, "Итог: обработано %d из %d достижимых клеток. Время: %d тактов.\n",
                 actors[0].processed + actors[1].processed, garden.reachable_count, tick);
    output_print(&out, "Полные логи можно посмотреть в файле garden.log в папке, из которой запущена программа.\n");
    if (close(out.log_fd) < 0) { perror("Закрытие журнала"); out.failed = true; }
    if (out.failed) { dprintf(STDERR_FILENO, "Ошибка записи результатов.\n"); result = 1; }
    return result;
}
