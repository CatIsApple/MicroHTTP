/* main.c — 명령줄 인자를 읽고 서버를 시작한다.
 * 사용법: microhttp [-q] [-r doc_root] [-c cgi_dir] [-w workers] [port]
 *
 * 레이어 구조에서의 위치: 프로그램 진입점. 설정(server_config)만 만들고 실제 일은 server_run(L6) 에 맡긴다.
 * 구성: parse_port(포트 검사), usage(도움말), main(옵션 해석 → 경로 정규화 → server_run) */
#define _XOPEN_SOURCE 700 /* realpath 는 POSIX 의 XSI 확장이라 _POSIX_C_SOURCE 만으로는 선언되지 않는다 */

#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* 문자열을 1~65535 포트 번호로 바꾼다. 숫자가 아니거나 범위를 벗어나면 -1 */
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

/* 사용법과 옵션 설명을 stderr 에 찍는다 */
static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [-q] [-r doc_root] [-c cgi_dir] [-w workers] [port]\n"
                    "  -q            정상 요청 로그 끄기\n"
                    "  -r doc_root   정적 파일 루트 (기본 ./www)\n"
                    "  -c cgi_dir    /cgi-bin/ 으로 실행할 스크립트 폴더 (기본 ./cgi-bin, 없으면 CGI 끔)\n"
                    "  -w workers    워커 스레드 수 (기본 8)\n"
                    "  port          기본 8080\n", prog);
}

/* 옵션 읽기 → doc_root, cgi_dir 을 realpath 로 정규화 → server_config 를 만들어 server_run.
 * 잘못된 인자는 2, 경로 오류·서버 시작 실패는 1 로 끝난다 */
int main(int argc, char **argv)
{
    uint16_t port = 8080;
    int quiet = 0;
    const char *root_arg = "www";
    const char *cgi_arg = NULL;
    long workers = 8;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0) {
            quiet = 1;
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            root_arg = argv[++i];
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            cgi_arg = argv[++i];
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            char *end;
            workers = strtol(argv[++i], &end, 10);
            if (*end != '\0' || workers < 1 || workers > 1024) {
                usage(argv[0]);
                return 2;
            }
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
    return server_run(&cfg, port, (size_t)workers, quiet) == 0 ? 0 : 1;
}
