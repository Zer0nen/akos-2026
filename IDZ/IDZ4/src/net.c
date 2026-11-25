#include "net.h"
#include "garden.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t interrupted;

static void on_signal(int number) { interrupted = number; }

int signals_init(void) {
    struct sigaction action = {.sa_handler = on_signal};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0) return -1;
    action.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &action, NULL);
}

long long monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int port_number(const char *arg, int minimum, int *port) {
    return parse_int(arg, port) == 0 && *port >= minimum && *port <= 65535 ? 0 : -1;
}

int tcp_open(const char *ip, int port, int listening) {
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((unsigned short)port)};
    if (inet_pton(AF_INET, ip, &address.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    if (listening) {
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) == 0 &&
            bind(fd, (struct sockaddr *)&address, sizeof address) == 0 && listen(fd, 24) == 0 &&
            fcntl(fd, F_SETFL, O_NONBLOCK) == 0) return fd;
    } else {
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes) == 0 &&
            connect(fd, (struct sockaddr *)&address, sizeof address) == 0) return fd;
    }
    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

/* 1: строка готова, 0: пока нет данных, -1: разрыв или ошибка протокола. */
int read_line(int fd, Reader *reader, char line[LINE_CAP]) {
    for (;;) {
        char *end = memchr(reader->data, '\n', reader->used);
        if (end) {
            size_t length = (size_t)(end - reader->data);
            if (memchr(reader->data, '\0', length)) return -1;
            memcpy(line, reader->data, length);
            line[length] = '\0';
            reader->used -= length + 1;
            memmove(reader->data, end + 1, reader->used);
            return 1;
        }
        if (reader->used == sizeof reader->data) return -1;
        ssize_t count = recv(fd, reader->data + reader->used, sizeof reader->data - reader->used, MSG_DONTWAIT);
        if (count > 0) reader->used += (size_t)count;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
        else return -1;
    }
}

int send_text(int fd, const char *text) {
    size_t size = strlen(text);
    while (size) {
        ssize_t sent = send(fd, text, size, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR && !interrupted) continue;
        if (sent <= 0) return -1;
        text += sent;
        size -= (size_t)sent;
    }
    return 0;
}
