/* main.c — 명령줄 인자를 읽고 서버를 시작한다.
 * 사용법: microhttp [-q] [port]   (-q: 정상 요청 로그 끄기, port 기본값 8080) */
#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_port(const char *s, uint16_t *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end != '\0' || v < 1 || v > 65535)
        return -1;
    *out = (uint16_t)v;
    return 0;
}

int main(int argc, char **argv)
{
    uint16_t port = 8080;
    int quiet = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0) {
            quiet = 1;
        } else if (parse_port(argv[i], &port) < 0) {
            fprintf(stderr, "usage: %s [-q] [port]\n", argv[0]);
            return 2;
        }
    }
    return server_run(port, quiet) == 0 ? 0 : 1;
}
