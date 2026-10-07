#ifndef GARDEN_H
#define GARDEN_H

#include <stdbool.h>

#define MAX_SIDE 30
#define MAX_CELLS (MAX_SIDE * MAX_SIDE)

/* Параметры читаются из файла вместе с картой сада. */
typedef struct {
    int move_ticks[2], work_ticks;         /* Длительности действий в модельных тактах */
    int start[2], delay_ms;                /* Номера стартовых клеток и пауза отображения */
    bool route2_columns, detours;
} Config;

typedef struct {
    int rows, cols, open_count, reachable_count;
    char cells[MAX_CELLS];                 /* # — препятствие, . — свободно, + — обработано */
    bool reachable[2][MAX_CELLS];          /* Достижимость из старта каждого садовника */
} Garden;

typedef enum { READY, MOVING, PROCESSING, WAITING, FINISHED } State;

typedef struct {
    int id, position, destination;         /* Участник, текущая клетка и назначение перехода */
    int due_tick, processed;               /* Время окончания действия и число обработанных клеток */
    int route[MAX_CELLS], cursor;          /* Змейка и индекс следующей цели */
    State state;
} Gardener;

typedef struct {
    int log_fd;
    bool failed, terminal, animate, compact, short_view;
    int event_count;
    char events[4][256];                  /* Последние события для текущего кадра */
} Output;

int garden_load(Garden *garden, Config *config, const char *path);
int garden_next_step(const Garden *garden, const Gardener actors[2],
                     int id, int target, bool detours);
void garden_print(const Garden *garden, const Gardener actors[2], Output *out);

void gardener_init(Gardener *actor, const Garden *garden, const Config *config, int id);
void gardener_complete(Gardener *actor, Garden *garden, int tick, Output *out);
bool gardener_step(Gardener *actor, Garden *garden, const Gardener actors[2],
                   const Config *config, int tick, Output *out);
void gardener_finish(Gardener *actor, int tick, Output *out);
void output_print(Output *out, const char *format, ...);
void output_event(Output *out, const char *format, ...);
void output_control(Output *out, const char *sequence);

#endif
