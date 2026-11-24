/* test_static_file.c — static_resolve (경로 해석, 순수 함수), http_parse_date,
 * static_lookup (임시 디렉터리에 파일을 만들어 실제 openat/fstat 경로) 단위 테스트. */

#define _POSIX_C_SOURCE 200809L /* mkdtemp, mkfifo, symlink */

#include "http_response.h" /* http_parse_date, http_format_date */
#include "static_file.h"
#include "test.h"

#include <fcntl.h>    /* open */
#include <stdio.h>    /* snprintf */
#include <stdlib.h>   /* mkdtemp */
#include <string.h>   /* strstr, strlen */
#include <sys/stat.h> /* mkdir, mkfifo, chmod */
#include <time.h>     /* time */
#include <unistd.h>   /* write, close, unlink, rmdir, geteuid */

static char g_out[STATIC_PATH_MAX];

static int res(const char *uri) /* static_resolve 결과 코드 (경로는 g_out) */
{
    g_out[0] = '\0';
    return static_resolve(uri, g_out, sizeof(g_out), NULL);
}

static void write_file(const char *dir, const char *name, const char *content)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        if (write(fd, content, strlen(content)) < 0) { /* 실패해도 아래 CHECK 가 잡음 */
        }
        close(fd);
    }
}

int main(void)
{
    /* ==== static_resolve: 정상 ==== */
    CHECK_INT(res("/"), 0);
    CHECK_STR(g_out, "index.html");
    CHECK_INT(res("/a.html"), 0);
    CHECK_STR(g_out, "a.html");
    CHECK_INT(res("/css/style.css"), 0);
    CHECK_STR(g_out, "css/style.css");
    CHECK_INT(res("/docs/"), 0);                              /* 디렉터리 요청 → index */
    CHECK_STR(g_out, "docs/index.html");
    CHECK_INT(res("/docs"), 0);                               /* '/' 없음 → 그대로 (디렉터리면 lookup 이 301) */
    CHECK_STR(g_out, "docs");

    /* 쿼리·조각 무시 */
    CHECK_INT(res("/a.html?x=1&y=../../etc"), 0);
    CHECK_STR(g_out, "a.html");
    CHECK_INT(res("/?q"), 0);
    CHECK_STR(g_out, "index.html");
    CHECK_INT(res("/a#frag"), 0);
    CHECK_STR(g_out, "a");

    /* 퍼센트 디코딩 */
    CHECK_INT(res("/hello%20world.txt"), 0);
    CHECK_STR(g_out, "hello world.txt");
    CHECK_INT(res("/%ED%95%9C%EA%B8%80.html"), 0);           /* UTF-8 "한글" */
    CHECK_STR(g_out, "\xED\x95\x9C\xEA\xB8\x80.html");
    CHECK_INT(res("/a%2Fb"), 0);                              /* %2F 도 구분자 */
    CHECK_STR(g_out, "a/b");
    CHECK_INT(res("/a+b"), 0);                                /* 경로에서 '+' 는 그냥 '+' */
    CHECK_STR(g_out, "a+b");

    /* 점 세그먼트 정리 */
    CHECK_INT(res("/a/./b"), 0);
    CHECK_STR(g_out, "a/b");
    CHECK_INT(res("/a//b"), 0);
    CHECK_STR(g_out, "a/b");
    CHECK_INT(res("/a/b/../c"), 0);
    CHECK_STR(g_out, "a/c");
    CHECK_INT(res("/a/.."), 0);                               /* 루트로 돌아옴 → 디렉터리 */
    CHECK_STR(g_out, "index.html");
    CHECK_INT(res("/a/."), 0);
    CHECK_STR(g_out, "a/index.html");
    {
        int d = 0;
        CHECK_INT(static_resolve("/x/", g_out, sizeof(g_out), &d), 0);
        CHECK_INT(d, 1);
        CHECK_INT(static_resolve("/x", g_out, sizeof(g_out), &d), 0);
        CHECK_INT(d, 0);
    }

    /* absolute-form */
    CHECK_INT(res("http://example.com/a/b.png?x"), 0);
    CHECK_STR(g_out, "a/b.png");
    CHECK_INT(res("http://example.com:8080"), 0);
    CHECK_STR(g_out, "index.html");
    CHECK_INT(res("HTTP://h?x"), 0);
    CHECK_STR(g_out, "index.html");

    /* ==== static_resolve: 거절 ==== */
    CHECK_INT(res("/.."), 403);                               /* 루트 탈출 */
    CHECK_INT(res("/../etc/passwd"), 403);
    CHECK_INT(res("/a/../../etc/passwd"), 403);
    CHECK_INT(res("/%2e%2e/etc/passwd"), 403);                /* 인코딩 우회 */
    CHECK_INT(res("/%2E%2E%2Fetc%2Fpasswd"), 403);
    CHECK_INT(res("/a/%2e%2e/%2e%2e/x"), 403);
    CHECK_INT(res("/.git/config"), 403);                      /* 숨김 파일 */
    CHECK_INT(res("/.env"), 403);
    CHECK_INT(res("/a/.htpasswd"), 403);
    CHECK_INT(res("/.well-known/acme"), 0);                   /* 예외 */
    CHECK_STR(g_out, ".well-known/acme");
    CHECK_INT(res("/a%00.png"), 400);                         /* NUL */
    CHECK_INT(res("/a%"), 400);                               /* 잘린 %XX */
    CHECK_INT(res("/a%4"), 400);
    CHECK_INT(res("/a%zz"), 400);
    CHECK_INT(res("a.html"), 400);                            /* '/' 로 시작 안 함 */
    CHECK_INT(res("*"), 400);
    CHECK_INT(res("://x/a"), 400);
    {
        char small[8];
        CHECK_INT(static_resolve("/abcdefgh", small, sizeof(small), NULL), 414); /* 결과가 버퍼에 안 들어감 */
        CHECK_INT(static_resolve("/", small, sizeof(small), NULL), 414);         /* "index.html" 도 */
        CHECK_INT(static_resolve("/abc", small, sizeof(small), NULL), 0);
        CHECK_STR(small, "abc");
        static char huge[STATIC_PATH_MAX + 16];
        huge[0] = '/';
        memset(huge + 1, 'a', sizeof(huge) - 2);
        huge[sizeof(huge) - 1] = '\0';
        CHECK_INT(res(huge), 414);
    }

    /* ==== http_parse_date ==== */
    {
        time_t t;
        CHECK_INT(http_parse_date("Sun, 06 Nov 1994 08:49:37 GMT", &t), 0);
        CHECK_INT(t, 784111777);                              /* RFC 7231 예제 시각 */
        CHECK_INT(http_parse_date("Thu, 01 Jan 1970 00:00:00 GMT", &t), 0);
        CHECK_INT(t, 0);
        CHECK_INT(http_parse_date("Tue, 29 Feb 2000 12:00:00 GMT", &t), 0); /* 400 년 윤년 */
        CHECK_INT(t, 951825600);
        /* format → parse 왕복 */
        char buf[64];
        time_t samples[] = {0, 784111777, 951825600, 1700000000, 2000000000};
        for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
            http_format_date(buf, sizeof(buf), samples[i]);
            CHECK(http_parse_date(buf, &t) == 0 && t == samples[i]);
        }
        CHECK_INT(http_parse_date("Sun, 06 Nov 1994 08:49:37 UTC", &t), -1);
        CHECK_INT(http_parse_date("Sunday, 06-Nov-94 08:49:37 GMT", &t), -1); /* 옛 형식 */
        CHECK_INT(http_parse_date("Sun Nov  6 08:49:37 1994", &t), -1);
        CHECK_INT(http_parse_date("Sun, 06 Foo 1994 08:49:37 GMT", &t), -1);
        CHECK_INT(http_parse_date("Sun, 30 Feb 1994 08:49:37 GMT", &t), -1);
        CHECK_INT(http_parse_date("Sun, 29 Feb 1900 08:49:37 GMT", &t), -1);  /* 1900 은 윤년 아님 */
        CHECK_INT(http_parse_date("Sun, 06 Nov 1994 24:00:00 GMT", &t), -1);
        CHECK_INT(http_parse_date("Sun, 06 Nov 1994 08:49:37 GMT ", &t), -1);
        CHECK_INT(http_parse_date("", &t), -1);
    }

    /* ==== static_lookup: 임시 문서 루트 ==== */
    char dir[] = "/tmp/microhttp_static_XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char p[512];
    write_file(dir, "index.html", "<h1>hi</h1>");
    write_file(dir, "a.css", "body{}");
    snprintf(p, sizeof(p), "%s/sub", dir);
    mkdir(p, 0755);
    write_file(dir, "sub/index.html", "sub");
    snprintf(p, sizeof(p), "%s/empty", dir);
    mkdir(p, 0755);                                          /* index.html 없는 디렉터리 */
    snprintf(p, sizeof(p), "%s/fifo", dir);
    mkfifo(p, 0644);
    write_file(dir, "secret.txt", "no");
    snprintf(p, sizeof(p), "%s/secret.txt", dir);
    chmod(p, 0000);

    int root = static_open_root(dir);
    CHECK(root >= 0);
    static_result r;

    static_lookup(root, "/", NULL, &r);
    CHECK_INT(r.status, 200);
    CHECK_INT(r.size, 11);
    CHECK_STR(r.content_type, "text/html; charset=utf-8");
    CHECK(r.fd >= 0);
    CHECK(strncmp(r.headers, "Last-Modified: ", 15) == 0);
    if (r.fd >= 0) {
        char b[32] = {0};
        CHECK_INT(read(r.fd, b, sizeof(b)), 11);
        CHECK_STR(b, "<h1>hi</h1>");
        close(r.fd);
    }

    static_lookup(root, "/a.css?v=3", NULL, &r);
    CHECK_INT(r.status, 200);
    CHECK_STR(r.content_type, "text/css; charset=utf-8");
    if (r.fd >= 0)
        close(r.fd);

    /* 304: Last-Modified 값을 그대로 If-Modified-Since 로 */
    static_lookup(root, "/a.css", NULL, &r);
    char lm[64];
    snprintf(lm, sizeof(lm), "%.*s", (int)(strlen(r.headers) - 17), r.headers + 15); /* "Last-Modified: " ~ "\r\n" */
    if (r.fd >= 0)
        close(r.fd);
    static_lookup(root, "/a.css", lm, &r);
    CHECK_INT(r.status, 304);
    CHECK_INT(r.fd, -1);
    CHECK(strstr(r.headers, "Last-Modified: ") != NULL);
    static_lookup(root, "/a.css", "Thu, 01 Jan 1970 00:00:00 GMT", &r); /* 그 뒤로 바뀜 → 200 */
    CHECK_INT(r.status, 200);
    if (r.fd >= 0)
        close(r.fd);
    static_lookup(root, "/a.css", "Fri, 31 Dec 9999 23:59:59 GMT", &r); /* 미래 날짜는 무시 → 200 */
    CHECK_INT(r.status, 200);
    if (r.fd >= 0)
        close(r.fd);
    static_lookup(root, "/a.css", "garbage", &r);            /* 해석 불가 → 무시 */
    CHECK_INT(r.status, 200);
    if (r.fd >= 0)
        close(r.fd);

    /* 디렉터리 */
    static_lookup(root, "/sub/", NULL, &r);
    CHECK_INT(r.status, 200);
    CHECK_INT(r.size, 3);
    if (r.fd >= 0)
        close(r.fd);
    static_lookup(root, "/sub", NULL, &r);                    /* '/' 없음 → 301 */
    CHECK_INT(r.status, 301);
    CHECK_STR(r.headers, "Location: /sub/\r\n");
    CHECK_INT(r.fd, -1);
    static_lookup(root, "/sub?a=1", NULL, &r);                /* 쿼리 유지 */
    CHECK_STR(r.headers, "Location: /sub/?a=1\r\n");
    /* 앞 '/' 여러 개 → 하나로. "//sub/" 그대로 주면 브라우저가 호스트 "sub" 로 가는 열린 리다이렉트 */
    static_lookup(root, "//sub", NULL, &r);
    CHECK_INT(r.status, 301);
    CHECK_STR(r.headers, "Location: /sub/\r\n");
    static_lookup(root, "///sub?a=1", NULL, &r);
    CHECK_STR(r.headers, "Location: /sub/?a=1\r\n");
    static_lookup(root, "http://h//sub", NULL, &r);           /* 절대형 URI 의 경로도 같음 */
    CHECK_STR(r.headers, "Location: /sub/\r\n");
    static_lookup(root, "/empty/", NULL, &r);                /* index 없음, 목록 안 보여 줌 */
    CHECK_INT(r.status, 404);

    /* 오류 */
    static_lookup(root, "/nope.html", NULL, &r);
    CHECK_INT(r.status, 404);
    CHECK_INT(r.fd, -1);
    static_lookup(root, "/a.css/x", NULL, &r);                /* 중간이 파일 → ENOTDIR → 404 */
    CHECK_INT(r.status, 404);
    static_lookup(root, "/../etc/passwd", NULL, &r);
    CHECK_INT(r.status, 403);
    static_lookup(root, "/fifo", NULL, &r);                   /* FIFO: open 에서 멈추지 않고 403 */
    CHECK_INT(r.status, 403);
    CHECK_INT(r.fd, -1);
    if (geteuid() != 0) {                                    /* root 는 권한 무시 → 검사 생략 */
        static_lookup(root, "/secret.txt", NULL, &r);
        CHECK_INT(r.status, 403);
    }
    static_lookup(root, "/a%00", NULL, &r);
    CHECK_INT(r.status, 400);

    /* 정리 */
    close(root);
    chmod(p, 0644);
    const char *files[] = {"index.html", "a.css", "sub/index.html", "fifo", "secret.txt"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(p, sizeof(p), "%s/%s", dir, files[i]);
        unlink(p);
    }
    snprintf(p, sizeof(p), "%s/sub", dir);
    rmdir(p);
    snprintf(p, sizeof(p), "%s/empty", dir);
    rmdir(p);
    rmdir(dir);

    return TEST_REPORT();
}
