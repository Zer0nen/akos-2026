#include "garden.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int id;
    if (argc != 4 || (strcmp(argv[1], "shm") && strcmp(argv[1], "mq")) ||
        parse_int(argv[3], &id) < 0 || id < 1 || id > 2) {
        dprintf(STDERR_FILENO, "Запуск: gardener shm|mq имя_сеанса 1|2\n"); return 1;
    }
    if (signals_init() < 0) { perror("Сигналы"); return 1; }
    Ipc ipc;
    if (ipc_open(&ipc, !strcmp(argv[1], "shm") ? SHARED : QUEUE, argv[2], NULL) < 0) {
        perror("Подключение IPC"); return 1;
    }
    int result = worker_run(&ipc, id - 1);
    ipc_close(&ipc);
    return result;
}
