/* main.c — 명령줄 인자를 읽고 서버를 시작한다.
 * 사용법: microhttp [-q] [-r doc_root] [-c cgi_dir] [port] */
#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

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

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [-q] [-r doc_root] [-c cgi_dir] [port]\n"
                    "  -q            정상 요청 로그 끄기\n"
                    "  -r doc_root   정적 파일 루트 (기본 ./www)\n"
                    "  -c cgi_dir    /cgi-bin/ 으로 실행할 스크립트 폴더 (기본 ./cgi-bin, 없으면 CGI 끔)\n"
                    "  port          기본 8080\n", prog);
}

int main(int argc, char **argv)
{
    uint16_t port = 8080;
    int quiet = 0;
    const char *root_arg = "www";
    const char *cgi_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0) {
            quiet = 1;
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            root_arg = argv[++i];
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            cgi_arg = argv[++i];
        } else if (parse_port(argv[i], &port) < 0) {
            usage(argv[0]);
            return 2;
        }
    }

    /* 루트를 절대 경로로 정규화해 둔다. 핸들러가 realpath 결과와 앞부분을 비교하기 때문 */
    char root[PATH_MAX];
    if (!realpath(root_arg, root)) {
        fprintf(stderr, "doc root %s: %s\n", root_arg, strerror(errno));
        return 1;
    }
    server_config cfg = {.doc_root = root, .cgi_prefix = NULL, .cgi_dir = NULL};

    /* CGI 폴더: -c 로 줬는데 없으면 오류, 안 줬으면 ./cgi-bin 이 있을 때만 켠다 */
    char cgi[PATH_MAX];
    if (realpath(cgi_arg ? cgi_arg : "cgi-bin", cgi)) {
        cfg.cgi_dir = cgi;
        cfg.cgi_prefix = "/cgi-bin/";
    } else if (cgi_arg) {
        fprintf(stderr, "cgi dir %s: %s\n", cgi_arg, strerror(errno));
        return 1;
    }
    return server_run(&cfg, port, quiet) == 0 ? 0 : 1;
}
