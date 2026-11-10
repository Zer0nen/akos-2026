#include "../src/garden.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void pause_briefly(void) {
    struct timespec pause = {0, 5000000L}; nanosleep(&pause, NULL);
}
static bool contains(const char *file, const char *text, int count) {
    FILE *stream = fopen(file, "r");
    if (!stream) return false;
    char line[512]; int matches = 0;
    while (fgets(line, sizeof line, stream)) if (strstr(line, text)) ++matches;
    fclose(stream);
    return matches >= count;
}
static bool wait_text(const char *file, const char *text, int count) {
    for (int i = 0; i < 600; ++i) {
        if (contains(file, text, count)) return true;
        pause_briefly();
    }
    return false;
}
static pid_t start(const char *program, const char *mode, const char *session, const char *arg) {
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("out.txt", O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) _exit(127);
        close(fd);
        execl(program, program, mode, session, arg, (char *)NULL);
        _exit(127);
    }
    return pid;
}

int main(int argc, char **argv) {
    if (argc != 5) return 1;
    bool zombie = !strcmp(argv[4], "zombie");
    char directory[] = "/tmp/akos3-zombie.XXXXXX", session[40], name[128];
    if (!mkdtemp(directory) || chdir(directory) < 0) return 1;
    snprintf(session, sizeof session, "zombie_%ld", (long)getpid());
    int number = !strcmp(argv[3], "shm") ? SHARED : QUEUE;
    FILE *input = fopen("input.txt", "w");
    if (!input) return 1;
    fputs("1 2\n1 2 100\nC 1\n0 0 0 1\n100\n..\n", input); fclose(input);
    pid_t server = start(argv[1], argv[3], session, "input.txt"), workers[2] = {0};
    bool ok = false, collected = false;
    if (server <= 0 || !wait_text("out.txt", "READY", 1)) goto cleanup;
    for (int id = 0; id < 2; ++id) {
        workers[id] = start(argv[2], argv[3], session, id == 0 ? "1" : "2");
        if (workers[id] <= 0) goto cleanup;
    }
    if (!wait_text("garden.log", "начал обработку", 2) || kill(workers[0], SIGKILL) < 0) goto cleanup;
    /* WNOWAIT намеренно оставляет завершившегося садовника zombie до конца проверки. */
    siginfo_t info;
    if (waitid(P_PID, workers[0], &info, WEXITED | WNOWAIT) < 0 || info.si_code != CLD_KILLED ||
        kill(workers[0], 0) != 0) goto cleanup;
    if (!zombie) {
        if (waitpid(workers[0], NULL, 0) != workers[0]) goto cleanup;
        workers[0] = 0;
    }
    int status = 0;
    for (int i = 0; i < 600; ++i) {
        if (waitpid(server, &status, WNOHANG) == server) { collected = true; break; }
        pause_briefly();
    }
    if (!collected || !WIFEXITED(status) || WEXITSTATUS(status) != 1 ||
        (zombie && kill(workers[0], 0) != 0)) goto cleanup;
    snprintf(name, sizeof name, "/akos3_%u_%d_%s_state", (unsigned)getuid(), number, session);
    if (number == SHARED) {
        int fd = shm_open(name, O_RDWR, 0);
        if (fd >= 0) { close(fd); goto cleanup; }
    } else {
        mqd_t queue = mq_open(name, O_RDWR);
        if (queue != (mqd_t)-1) { mq_close(queue); goto cleanup; }
    }
    if (errno != ENOENT) goto cleanup;
    const char *suffix[] = {"lock", "wake0", "wake1"};
    for (int i = 0; i < 3; ++i) {
        snprintf(name, sizeof name, "/akos3_%u_%d_%s_%s", (unsigned)getuid(), number, session, suffix[i]);
        sem_t *sem = sem_open(name, 0);
        if (sem != SEM_FAILED) { sem_close(sem); goto cleanup; }
        if (errno != ENOENT) goto cleanup;
    }
    ok = true;
cleanup:
    if (!ok) fprintf(stderr, "%s regression failed; collected=%d\n", argv[4], collected);
    if (server > 0 && !collected) { kill(server, SIGKILL); waitpid(server, NULL, 0); }
    for (int id = 0; id < 2; ++id) if (workers[id] > 0) { kill(workers[id], SIGKILL); waitpid(workers[id], NULL, 0); }
    /* Очистка теста нужна и при запуске на старой, заведомо ошибочной программе. */
    const char *all[] = {"lock", "wake0", "wake1", "state"};
    for (int i = 0; i < 4; ++i) {
        snprintf(name, sizeof name, "/akos3_%u_%d_%s_%s", (unsigned)getuid(), number, session, all[i]);
        if (i < 3) sem_unlink(name);
        else if (number == SHARED) shm_unlink(name); else mq_unlink(name);
    }
    unlink("input.txt"); unlink("out.txt"); unlink("garden.log"); chdir("/tmp"); rmdir(directory);
    if (ok) printf("%s %s: controller detected death and removed IPC\n", argv[3], argv[4]);
    return !ok;
}
