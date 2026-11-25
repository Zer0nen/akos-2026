#ifndef GARDEN4_H
#define GARDEN4_H

#include <stdbool.h>

#define SIDE 30
#define CELLS (SIDE * SIDE)

typedef enum { READY, MOVING, PROCESSING, WAITING, FINISHED, OFFLINE } Status;
typedef struct {
    int rows, cols, move[2], work, start[2], unit_ms;
    bool columns, detours;
} Config;
typedef struct {
    int position, destination, processed;
    Status status;
} Actor;
typedef struct {
    Config cfg;
    int reachable_count;
    char cells[CELLS];
    bool reachable[2][CELLS];
    Actor actors[2];
} State;

int parse_int(const char *text, int *value);
int load_garden(State *s, const char *path);
void make_route(const State *s, int id, int route[CELLS]);
int next_step(const State *s, int id, int target);
int find_yield_cell(const State *s, int id, int other_target);
int draw_garden(int fd, int rows, int cols, const char *map, const int positions[2]);
int describe_remaining(int fd, int cols, const char *map);
const char *end_reason(int code);

#endif
