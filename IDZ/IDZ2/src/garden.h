#ifndef GARDEN_H
#define GARDEN_H

#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#ifdef USE_CONDVAR
#define SYNC_NAME "мьютекс и условные переменные"
#else
#include <semaphore.h>
#define SYNC_NAME "мьютекс и семафоры"
#endif

#define MAX_SIDE 30
#define MAX_CELLS (MAX_SIDE * MAX_SIDE)

typedef struct {
    int move[2], work, start[2], unit_ms;
    bool columns, detours;
} Config;

typedef struct {
    int rows, cols, open_count, reachable_count;
    char cells[MAX_CELLS];                 /* # — препятствие, . — свободно, + — обработано */
    bool reachable[2][MAX_CELLS];
} Garden;

typedef enum { READY, MOVING, PROCESSING, WAITING, FINISHED } State;
typedef struct Simulation Simulation;

typedef struct {
    Simulation *simulation;
    int id, position, destination, processed;
    int route[MAX_CELLS], cursor;
    State state;
} Gardener;

/* Все изменяемые поля общей модели защищены одним мьютексом. */
struct Simulation {
    Config cfg;
    Garden garden;
    Gardener actors[2];
    pthread_mutex_t mutex;
#ifdef USE_CONDVAR
    pthread_cond_t changed[2];
#else
    sem_t changed[2];
#endif
    unsigned revision;
    int finished, result, log_fd;
    bool stopped, failed, terminal;
    struct timespec started;
    char events[2][256];
};

int garden_load(Garden *g, Config *cfg, const char *path);
int garden_next_step(const Simulation *s, int id, int target);
void garden_draw(Simulation *s, int fd, bool colors);
int synchronization_init(Simulation *s);
void synchronization_destroy(Simulation *s);
void wake_waiters(Simulation *s);
void gardener_init(Simulation *s, int id);
void *gardener_run(void *argument);
void event(Simulation *s, int id, const char *format, ...);
bool action_pause(Simulation *s, int units);

#endif
