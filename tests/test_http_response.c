/* test_http_response.c — L3 단위 테스트.
 * socketpair 한쪽으로 http_response_send 를 하고, 다른 쪽에서 받은 바이트를 검사한다.
 *
 * 구성: send_and_capture 가 받은 응답 전체를 전역 out 에 담고, body_of 가 그 안의 본문 위치를 찾는다.
 * 테스트 함수마다 본문 종류(없음/메모리/파일/owned), HEAD, 오류 페이지, 헤더 검사 중 하나를 확인한다. */
#define _POSIX_C_SOURCE 200809L

#include "http_response.h"
#include "tcp.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;
#define EXPECT(c)                                                        \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                  \
        }                                                                \
    } while (0)

static char out[256 * 1024];
static size_t out_len;

/* res 를 보내고, 받은 전체를 out 에 NUL 로 끝나게 담는다 */
static int send_and_capture(http_response *res, int head_only)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return -1;
    /* 큰 본문은 소켓 버퍼보다 커서, 보내는 쪽과 받는 쪽이 동시에 돌아야 한다 → 자식 프로세스가 보낸다 */
    pid_t pid = fork();
    if (pid == 0) {
        close(sv[1]);
        _exit(http_response_send(sv[0], res, head_only, tcp_now_ms() + 2000) == 0 ? 0 : 1);
    }
    close(sv[0]);
    out_len = 0;
    ssize_t n;
    while ((n = tcp_recv_some(sv[1], out + out_len, sizeof out - 1 - out_len, tcp_now_ms() + 2000)) > 0)
        out_len += (size_t)n;
    out[out_len] = '\0';
    close(sv[1]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

/* 마지막으로 받은 응답(out)에서 빈 줄 뒤 본문의 시작. 빈 줄이 없으면 NULL */
static const char *body_of(void)
{
    char *p = strstr(out, "\r\n\r\n");
    return p ? p + 4 : NULL;
}

/* 알려진 상태 코드의 문구와, 모르는 코드의 "Unknown" */
static void status_texts(void)
{
    EXPECT(strcmp(http_status_text(200), "OK") == 0);
    EXPECT(strcmp(http_status_text(404), "Not Found") == 0);
    EXPECT(strcmp(http_status_text(503), "Service Unavailable") == 0);
    EXPECT(strcmp(http_status_text(799), "Unknown") == 0);
}

/* 메모리 본문: 상태 줄, 자동 헤더(Content-Length, Connection, Date, Server), 추가 헤더, 본문 */
static void memory_body(void)
{
    http_response res;
    http_response_init(&res, 200);
    http_response_body_mem(&res, "hello", 5, "text/plain");
    EXPECT(http_response_header(&res, "X-Test", "1") == 0);
    EXPECT(send_and_capture(&res, 0) == 0);
    EXPECT(strncmp(out, "HTTP/1.1 200 OK\r\n", 17) == 0);
    EXPECT(strstr(out, "\r\nContent-Length: 5\r\n") != NULL);
    EXPECT(strstr(out, "\r\nContent-Type: text/plain\r\n") != NULL);
    EXPECT(strstr(out, "\r\nConnection: keep-alive\r\n") != NULL);
    EXPECT(strstr(out, "\r\nDate: ") != NULL && strstr(out, " GMT\r\n") != NULL);
    EXPECT(strstr(out, "\r\nServer: MicroHTTP\r\n") != NULL);
    EXPECT(strstr(out, "\r\nX-Test: 1\r\n") != NULL);
    EXPECT(body_of() && strcmp(body_of(), "hello") == 0);
    http_response_free(&res);
}

/* HEAD: Content-Length 는 그대로 알리고 본문은 보내지 않음. keep_alive = 0 이면 Connection: close */
static void head_only_and_close(void)
{
    http_response res;
    http_response_init(&res, 200);
    res.keep_alive = 0;
    http_response_body_mem(&res, "hello", 5, "text/plain");
    EXPECT(send_and_capture(&res, 1) == 0);
    EXPECT(strstr(out, "\r\nContent-Length: 5\r\n") != NULL); /* 길이는 GET 과 같게 알려 준다 */
    EXPECT(strstr(out, "\r\nConnection: close\r\n") != NULL);
    EXPECT(body_of() && *body_of() == '\0');                  /* 본문은 안 보냄 */
}

/* 본문이 없는 응답(204)은 Content-Length: 0 */
static void no_body(void)
{
    http_response res;
    http_response_init(&res, 204);
    EXPECT(send_and_capture(&res, 0) == 0);
    EXPECT(strncmp(out, "HTTP/1.1 204 No Content\r\n", 25) == 0);
    EXPECT(strstr(out, "\r\nContent-Length: 0\r\n") != NULL);
}

/* http_response_error: 이전 헤더를 버리고 HTML 오류 페이지와 맞는 Content-Length 를 보냄 */
static void error_page(void)
{
    http_response res;
    http_response_init(&res, 200);
    http_response_header(&res, "X-Gone", "yes");
    http_response_error(&res, 404);
    EXPECT(send_and_capture(&res, 0) == 0);
    EXPECT(strncmp(out, "HTTP/1.1 404 Not Found\r\n", 24) == 0);
    EXPECT(strstr(out, "text/html") != NULL);
    EXPECT(strstr(out, "X-Gone") == NULL); /* 오류로 바꾸면 이전 헤더는 버린다 */
    EXPECT(body_of() && strstr(body_of(), "<h1>404 Not Found</h1>") != NULL);
    char want[64];
    snprintf(want, sizeof want, "\r\nContent-Length: %zu\r\n", strlen(body_of()));
    EXPECT(strstr(out, want) != NULL);
}

/* CR/LF 가 든 헤더 이름·값은 거절하고, 헤더 배열이 가득 차면 -1 */
static void header_injection_rejected(void)
{
    http_response res;
    http_response_init(&res, 200);
    EXPECT(http_response_header(&res, "Location", "/a\r\nSet-Cookie: x=1") == -1);
    EXPECT(http_response_header(&res, "Bad\nName", "v") == -1);
    EXPECT(res.n_headers == 0);
    for (int i = 0; i < HTTP_RESPONSE_MAX_HEADERS; i++)
        EXPECT(http_response_header(&res, "X", "v") == 0);
    EXPECT(http_response_header(&res, "X", "v") == -1); /* 가득 참 */
}

/* header_copy 는 값을 복사해 두므로 원본을 바꿔도 그대로이고, scratch 를 넘으면 -1 */
static void header_copy(void)
{
    http_response res;
    http_response_init(&res, 301);
    char local[32];
    strcpy(local, "/dir/");
    EXPECT(http_response_header_copy(&res, "Location", local) == 0);
    strcpy(local, "XXXX"); /* 원본을 바꿔도 복사본은 그대로 */
    EXPECT(send_and_capture(&res, 0) == 0);
    EXPECT(strstr(out, "\r\nLocation: /dir/\r\n") != NULL);
    char big[600];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    EXPECT(http_response_header_copy(&res, "X", big) == -1); /* scratch 초과 */
}

/* 파일 본문을 64KB 조각으로 나눠 끝까지 보내고, free 가 fd 를 닫으며 두 번 불러도 안전한지 */
static void file_body(void)
{
    char path[] = "/tmp/microhttp_test_XXXXXX";
    int fd = mkstemp(path);
    EXPECT(fd >= 0);
    unlink(path);
    static char data[150 * 1024]; /* 64KB 조각 여러 개로 나눠 보내지는지 */
    for (size_t i = 0; i < sizeof data; i++)
        data[i] = (char)('a' + i % 26);
    EXPECT(write(fd, data, sizeof data) == (ssize_t)sizeof data);

    http_response res;
    http_response_init(&res, 200);
    http_response_body_file(&res, fd, (off_t)sizeof data, "text/plain");
    EXPECT(send_and_capture(&res, 0) == 0);
    EXPECT(strstr(out, "\r\nContent-Length: 153600\r\n") != NULL);
    const char *b = body_of();
    EXPECT(b && (size_t)(out + out_len - b) == sizeof data && memcmp(b, data, sizeof data) == 0);
    http_response_free(&res);
    EXPECT(fcntl(fd, F_GETFD) == -1); /* free 가 파일을 닫았다 */
    http_response_free(&res);          /* 두 번 불러도 안전 */
}

/* owned 본문: malloc 한 버퍼의 일부를 보내고 free 가 버퍼를 해제하는지 (ASan 으로 누수 확인) */
static void owned_body(void)
{
    char *base = malloc(16);
    memcpy(base, "HDR:body", 8);
    http_response res;
    http_response_init(&res, 200);
    http_response_body_owned(&res, base, base + 4, 4, NULL);
    EXPECT(send_and_capture(&res, 0) == 0);
    EXPECT(body_of() && strcmp(body_of(), "body") == 0);
    http_response_free(&res); /* ASan 으로 누수가 없는지 확인 */
}

/* 모든 테스트를 실행하고, 실패가 있으면 개수를 찍고 1 을 돌려준다 */
int main(void)
{
    status_texts();
    memory_body();
    head_only_and_close();
    no_body();
    error_page();
    header_injection_rejected();
    header_copy();
    file_body();
    owned_body();
    if (failures) {
        fprintf(stderr, "test_http_response: %d failed\n", failures);
        return 1;
    }
    printf("test_http_response: ok\n");
    return 0;
}
