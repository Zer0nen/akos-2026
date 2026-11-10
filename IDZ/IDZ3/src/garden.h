#ifndef GARDEN_H
#define GARDEN_H

#include <stdbool.h>
#include <semaphore.h>
#include <mqueue.h>
#include <signal.h>
#include <sys/types.h>

#define SIDE 30
#define CELLS (SIDE * SIDE)
#define MAGIC 0x47415233

typedef enum { FORK_NAMED, FORK_UNNAMED, SHARED, QUEUE } Mode;
typedef enum { READY, MOVING, PROCESSING, WAITING, FINISHED } Status;
typedef struct {
    int rows, cols, move[2], work, start[2], unit_ms;
    bool columns, detours;
} Config;
typedef struct {
    pid_t pid;
    int position, destination, processed;
    Status status;
} Actor;

/* Передаваемые данные не содержат указателей: подходят и для mmap, и для mq_send. */
typedef struct {
    int magic, reachable_count, result, finished;
    unsigned revision;
    bool stopped, failed;
    pid_t owner;
    Config cfg;
    char cells[CELLS];
    bool reachable[2][CELLS];
    Actor actors[2];
    char events[2][256], log_path[1024];
} State;

typedef struct {
    sem_t mutex, changed[2];              /* Только для варианта с sem_init(pshared=1). */
    State state;
} SharedMemory;

typedef struct {
    Mode mode;
    bool owner, resource_created;
    int fd, log_fd, initialized;
    mqd_t queue;
    SharedMemory *memory;
    sem_t *mutex, *changed[2];
    char base[96];
    State state;                         /* Локальный снимок под межпроцессной блокировкой. */
} Ipc;

extern volatile sig_atomic_t interrupted;
int signals_init(void);
int parse_int(const char *text, int *value);
int load_garden(State *s, const char *file);
void make_route(const State *s, int id, int route[CELLS]);
int next_step(const State *s, int id, int target);
void event(Ipc *ipc, int id, const char *format, ...);
int draw(const State *s, int fd);
int summary(const State *s, int fd);
int ipc_open(Ipc *ipc, Mode mode, const char *session, const State *initial);
int ipc_lock(Ipc *ipc);
int ipc_unlock(Ipc *ipc);
void ipc_notify(Ipc *ipc);
int ipc_wait(Ipc *ipc, int id);           /* На входе и выходе блокировка захвачена. */
void ipc_close(Ipc *ipc);
int worker_run(Ipc *ipc, int id);

#endif
