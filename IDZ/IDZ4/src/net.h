#ifndef GARDEN4_NET_H
#define GARDEN4_NET_H

#include <signal.h>
#include <stddef.h>

#define LINE_CAP 1024

/* TCP сохраняет порядок байтов, но не границы отправленных строк. */
typedef struct {
    char data[LINE_CAP];
    size_t used;
} Reader;

extern volatile sig_atomic_t interrupted;
int signals_init(void);
long long monotonic_ms(void);
int tcp_open(const char *ip, int port, int listening);
int port_number(const char *arg, int minimum, int *port);
int read_line(int fd, Reader *reader, char line[LINE_CAP]);
int send_text(int fd, const char *text);

#endif
