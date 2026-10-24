#include "garden.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Сигналы принимаются только главным потоком. Рабочие читают stopped под мьютексом. */
static volatile sig_atomic_t interrupted = 0;
static void stop_handler(int number) { interrupted = number; }

/* Вызывается под мьютексом: строки двух садовников не перемешиваются. */
void event(Simulation *s, int id, const char *format, ...) {
    char message[256];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed = (now.tv_sec - s->started.tv_sec) * 1000L +
                   (now.tv_nsec - s->started.tv_nsec) / 1000000L;
    snprintf(s->events[id], sizeof s->events[id], "Садовник %d: %.220s.", id + 1, message);
    if (dprintf(s->log_fd, "[%ld мс] %s\n", elapsed, s->events[id]) < 0) s->failed = true;
    if (!s->terminal && dprintf(STDOUT_FILENO, "[%ld мс] %s\n", elapsed, s->events[id]) < 0)
        s->failed = true;
}

bool action_pause(Simulation *s, int units) {
    int remaining = units * s->cfg.unit_ms;
    while (remaining > 0) {
        int part = remaining < 20 ? remaining : 20;
        struct timespec pause = {0, part * 1000000L};
        while (nanosleep(&pause, &pause) < 0 && errno == EINTR) {}
        remaining -= part;
        pthread_mutex_lock(&s->mutex);
        bool stopped = s->stopped;
        pthread_mutex_unlock(&s->mutex);
        if (stopped) return false;
    }
    return true;
}

static void show_frame(Simulation *s) {
    const char *states[] = {"готов", "перемещается", "обрабатывает", "ожидает", "завершил"};
    if (dprintf(STDOUT_FILENO, "\033[H\033[JОбработано: %d/%d\n",
                s->actors[0].processed + s->actors[1].processed, s->garden.reachable_count) < 0)
        s->failed = true;
    garden_draw(s, STDOUT_FILENO, true);
    for (int id = 0; id < 2; ++id)
        if (dprintf(STDOUT_FILENO, "Садовник %d: %s.\n%s\n", id + 1,
                    states[s->actors[id].state], s->events[id]) < 0) s->failed = true;
}

int main(int argc, char **argv) {
    if (argc > 2) { dprintf(STDERR_FILENO, "Запуск: gardeners [файл данных]\n"); return 1; }
    Simulation s = {0};
    if (garden_load(&s.garden, &s.cfg, argc == 2 ? argv[1] : "data/demo.txt") < 0) return 1;
    s.log_fd = open("garden.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (s.log_fd < 0) { perror("Журнал"); return 1; }
    if (synchronization_init(&s) < 0) { perror("Синхронизация"); close(s.log_fd); return 1; }
    for (int id = 0; id < 2; ++id) gardener_init(&s, id);
    s.terminal = isatty(STDOUT_FILENO);
    clock_gettime(CLOCK_MONOTONIC, &s.started);
    if (dprintf(s.log_fd, "ИДЗ №2: %s. Два рабочих потока.\nСад %dx%d: доступно %d, достижимо %d.\n",
                SYNC_NAME, s.garden.rows, s.garden.cols, s.garden.open_count,
                s.garden.reachable_count) < 0) s.failed = true;
    garden_draw(&s, s.log_fd, false);

    struct sigaction action = {0};
    action.sa_handler = stop_handler;
    sigemptyset(&action.sa_mask);
    sigset_t signals, previous_mask;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    int error = 0;
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0)
        error = errno;
    if (!error) error = pthread_sigmask(SIG_BLOCK, &signals, &previous_mask);
    if (error) {
        dprintf(STDERR_FILENO, "Настройка сигналов: %s\n", strerror(error));
        synchronization_destroy(&s); close(s.log_fd); return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    if (s.terminal && dprintf(STDOUT_FILENO, "\033[?1049h\033[2J\033[?25l") < 0) s.failed = true;
    pthread_t threads[2];
    int created = 0;
    /* Удерживаем мьютекс до создания обоих потоков, затем они действуют самостоятельно. */
    pthread_mutex_lock(&s.mutex);
    if (s.failed) s.stopped = true;
    for (; created < 2 && !s.stopped;) {
        error = pthread_create(&threads[created], NULL, gardener_run, &s.actors[created]);
        if (error) {
            dprintf(STDERR_FILENO, "Создание потока: %s\n", strerror(error));
            s.failed = s.stopped = true;
        } else ++created;
    }
    if (s.stopped) wake_waiters(&s);
    pthread_mutex_unlock(&s.mutex);
    error = pthread_sigmask(SIG_SETMASK, &previous_mask, NULL);
    if (error) {
        pthread_mutex_lock(&s.mutex);
        s.failed = s.stopped = true;
        wake_waiters(&s);
        pthread_mutex_unlock(&s.mutex);
    }
    for (;;) {
        pthread_mutex_lock(&s.mutex);
        if (interrupted && !s.stopped) {
            s.stopped = true;
            s.result = 128 + interrupted;
            wake_waiters(&s);
        }
        if (s.failed) { s.stopped = true; wake_waiters(&s); }
        bool finished = s.finished == created;
        if (s.terminal) show_frame(&s);
        pthread_mutex_unlock(&s.mutex);
        if (finished) break;
        struct timespec pause = {0, 50000000L};
        nanosleep(&pause, NULL);
    }
    for (int id = 0; id < created; ++id) {
        error = pthread_join(threads[id], NULL);
        if (error) { dprintf(STDERR_FILENO, "Ожидание потока: %s\n", strerror(error)); s.failed = true; }
    }
    if (s.terminal && dprintf(STDOUT_FILENO, "\033[0m\033[?25h\033[?1049l") < 0) s.failed = true;
    const char *reason = s.result >= 128 ? "прервано пользователем" :
                         s.result == 2 ? "невозможно продолжить обход" : s.failed ? "ошибка" : "сад обработан";
    for (int output = 0; output < 2; ++output) {
        int fd = output == 0 ? STDOUT_FILENO : s.log_fd;
        if (dprintf(fd, "\nЗавершение: %s.\n", reason) < 0) s.failed = true;
        garden_draw(&s, fd, output == 0 && s.terminal);
        for (int cell = 0; cell < s.garden.rows * s.garden.cols; ++cell) {
            if (s.garden.cells[cell] == '.' && dprintf(fd, "Необработанная клетка (%d,%d): %s.\n",
                cell / s.garden.cols, cell % s.garden.cols,
                s.garden.reachable[0][cell] || s.garden.reachable[1][cell] ?
                "обход не завершён" : "недостижима из обоих стартов") < 0) s.failed = true;
        }
        if (dprintf(fd, "Итог: обработано %d из %d достижимых клеток. По садовникам: %d и %d.\n",
                    s.actors[0].processed + s.actors[1].processed, s.garden.reachable_count,
                    s.actors[0].processed, s.actors[1].processed) < 0) s.failed = true;
    }
    if (dprintf(STDOUT_FILENO, "Полная история — garden.log в текущей папке.\n") < 0) s.failed = true;
    synchronization_destroy(&s);
    if (close(s.log_fd) < 0) s.failed = true;
    if (s.failed) { dprintf(STDERR_FILENO, "Ошибка выполнения или записи результатов.\n"); return 1; }
    return s.result;
}
