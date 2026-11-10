#include "garden.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void finish_missing(State *s, int id) {
    Actor *a = &s->actors[id];
    if (a->status == FINISHED) return;
    a->status = FINISHED; a->position = a->destination = -1; ++s->finished;
}

/* Также вызывается при ошибке IPC: ни один запущенный ребёнок не остаётся бесхозным. */
static void stop_children(pid_t pids[2], bool forked) {
    for (int id = 0; id < 2; ++id) if (pids[id] > 0) kill(pids[id], SIGTERM);
    for (int attempt = 0; attempt < 100; ++attempt) {
        bool alive = false;
        for (int id = 0; id < 2; ++id) {
            if (pids[id] <= 0) continue;
            if ((forked && waitpid(pids[id], NULL, WNOHANG) == pids[id]) ||
                (!forked && kill(pids[id], 0) < 0 && errno == ESRCH)) pids[id] = 0;
            else alive = true;
        }
        if (!alive) return;
        struct timespec pause = {0, 20000000L}; nanosleep(&pause, NULL);
    }
    for (int id = 0; id < 2; ++id) if (pids[id] > 0) {
        kill(pids[id], SIGKILL);
        if (forked) while (waitpid(pids[id], NULL, 0) < 0 && errno == EINTR) {}
    }
}

int main(int argc, char **argv) {
    Mode mode;
    char session[48];
    const char *input;
#ifdef FORK_MODE
    bool forked = true;
    if (argc > 2) { dprintf(2, "Запуск: gardeners_named|gardeners_unnamed [файл]\n"); return 1; }
    mode = FORK_MODE;
    snprintf(session, sizeof session, "fork_%ld", (long)getpid());
    input = argc == 2 ? argv[1] : "data/demo.txt";
#else
    bool forked = false;
    if (argc < 3 || argc > 4 || (strcmp(argv[1], "shm") && strcmp(argv[1], "mq")) || strlen(argv[2]) > 40) {
        dprintf(2, "Запуск: garden_server shm|mq имя_сеанса [файл]\n"); return 1;
    }
    mode = !strcmp(argv[1], "shm") ? SHARED : QUEUE;
    snprintf(session, sizeof session, "%s", argv[2]);
    input = argc == 4 ? argv[3] : "data/demo.txt";
#endif
    if (signals_init() < 0) { perror("Сигналы"); return 1; }
    State initial;
    if (load_garden(&initial, input) < 0) return 1;
    char cwd[900];
    if (!getcwd(cwd, sizeof cwd)) { perror("Папка запуска"); return 1; }
    snprintf(initial.log_path, sizeof initial.log_path, "%s/garden.log", cwd);
    Ipc ipc;
    if (ipc_open(&ipc, mode, session, &initial) < 0) { perror("Создание IPC"); return 1; }
    ipc.log_fd = open(initial.log_path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (ipc.log_fd < 0) { perror("Журнал"); ipc_close(&ipc); return 1; }
    if (dprintf(ipc.log_fd, "ИДЗ3, вариант 15. Режим %d, сеанс %s.\n", mode, session) < 0) {
        ipc_close(&ipc); return 1;
    }
    if (draw(&initial, ipc.log_fd) < 0) { ipc_close(&ipc); return 1; }
    bool terminal = isatty(STDOUT_FILENO), fatal = false;
    pid_t pids[2] = {0};
    if (forked) {
        for (int id = 0; id < 2; ++id) {
            pids[id] = fork();
            if (pids[id] == 0) {
                ipc.owner = false;
                int code = worker_run(&ipc, id);
                ipc_close(&ipc); _exit(code);
            }
            if (pids[id] < 0) { perror("fork"); pids[id] = 0; fatal = true; break; }
        }
    } else if (dprintf(STDOUT_FILENO, "READY: сеанс %s, режим %s. Запустите gardener для участников 1 и 2.\n", session, argv[1]) < 0) fatal = true;
    if (terminal && dprintf(STDOUT_FILENO, "\033[?1049h\033[2J\033[?25l") < 0) fatal = true;
    int stopping_rounds = 0;
    while (!fatal) {
        if (ipc_lock(&ipc) < 0) { perror("Чтение IPC"); fatal = true; break; }
        State *s = &ipc.state;
        if (interrupted) { s->stopped = true; s->result = 128 + interrupted; }
        if (s->failed) s->stopped = true;
        for (int id = 0; id < 2; ++id) {
            if (!forked && s->actors[id].pid) pids[id] = s->actors[id].pid;
            if (pids[id] <= 0) continue;
            int status = 0;
            bool gone = forked ? waitpid(pids[id], &status, WNOHANG) == pids[id] :
                        kill(pids[id], 0) < 0 && errno == ESRCH;
            if (gone) {
                if ((!s->stopped && s->actors[id].status != FINISHED) ||
                    (forked && (!WIFEXITED(status) || WEXITSTATUS(status))))
                    s->failed = s->stopped = true;
                finish_missing(s, id); pids[id] = 0;
            }
        }
        if (s->stopped) {
            for (int id = 0; id < 2; ++id) if (!pids[id]) finish_missing(s, id);
            ipc_notify(&ipc);
            if (++stopping_rounds > 40) fatal = true;
        }
        bool finished = s->finished == 2;
        State snapshot = *s;
        if (ipc_unlock(&ipc) < 0) { fatal = true; break; }
        if (terminal) {
            if (dprintf(STDOUT_FILENO, "\033[H\033[JОбработано %d/%d. 1/2 — садовники, + — обработано, # — препятствие.\n",
                    snapshot.actors[0].processed + snapshot.actors[1].processed, snapshot.reachable_count) < 0) fatal = true;
            if (draw(&snapshot, STDOUT_FILENO) < 0) fatal = true;
            for (int id = 0; id < 2; ++id) if (dprintf(STDOUT_FILENO, "%s\n", snapshot.events[id]) < 0) fatal = true;
        }
        if (finished) break;
        struct timespec pause = {0, 50000000L}; nanosleep(&pause, NULL);
    }
    if (fatal) {
        stop_children(pids, forked);
        ipc.state.failed = true;
        for (int id = 0; id < 2; ++id) finish_missing(&ipc.state, id);
    } else if (forked) {
        for (int id = 0; id < 2; ++id) if (pids[id] > 0) {
            int status;
            pid_t child;
            do { child = waitpid(pids[id], &status, 0); } while (child < 0 && errno == EINTR);
            if (child < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) ipc.state.failed = true;
        }
    }
    if (terminal && dprintf(STDOUT_FILENO, "\033[0m\033[?25h\033[?1049l") < 0) ipc.state.failed = true;
    if (summary(&ipc.state, STDOUT_FILENO) < 0) ipc.state.failed = true;
    if (summary(&ipc.state, ipc.log_fd) < 0) ipc.state.failed = true;
    if (dprintf(STDOUT_FILENO, "Полная история: %s\n", ipc.state.log_path) < 0) ipc.state.failed = true;
    int result = ipc.state.failed ? 1 : ipc.state.result;
    ipc_close(&ipc);
    return result;
}
