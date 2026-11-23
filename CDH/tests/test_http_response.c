/* test_http_response.c — http_reason / http_format_head 단위 테스트. */

#include "http_response.h" /* 테스트 대상 */
#include "test.h"          /* CHECK, CHECK_INT, CHECK_STR, TEST_REPORT */
#include <string.h>        /* strcmp, strlen */

int main(void)
{
    /* 이유 문구 표 */
    CHECK_STR(http_reason(200), "OK");
    CHECK_STR(http_reason(400), "Bad Request");
    CHECK_STR(http_reason(411), "Length Required");
    CHECK_STR(http_reason(413), "Payload Too Large");
    CHECK_STR(http_reason(414), "URI Too Long");
    CHECK_STR(http_reason(417), "Expectation Failed");
    CHECK_STR(http_reason(431), "Request Header Fields Too Large");
    CHECK_STR(http_reason(501), "Not Implemented");
    CHECK_STR(http_reason(505), "HTTP Version Not Supported");
    CHECK_STR(http_reason(299), "Unknown");                      /* 표에 없는 코드 */

    /* 응답 머리 */
    char out[256];
    int n = http_format_head(out, sizeof(out), 404, "text/html", 12, 0);
    CHECK_STR(out, "HTTP/1.1 404 Not Found\r\n"
                   "Content-Type: text/html\r\n"
                   "Content-Length: 12\r\n"
                   "Connection: close\r\n"
                   "\r\n");
    CHECK_INT(n, (int)strlen(out));                              /* 반환값 = 쓴 길이 */

    n = http_format_head(out, sizeof(out), 200, "text/plain", 0, 1);
    CHECK(n > 0 && strstr(out, "Connection: keep-alive\r\n") != NULL);

    /* 버퍼 부족 → -1 (잘린 응답을 보내지 않게) */
    char tiny[20];
    CHECK_INT(http_format_head(tiny, sizeof(tiny), 200, "text/plain", 0, 0), -1);
    n = http_format_head(out, sizeof(out), 200, "text/plain", 0, 0); /* 정확한 길이 계산 */
    CHECK_INT(http_format_head(out, (size_t)n, 200, "text/plain", 0, 0), -1);     /* '\0' 자리가 없음 */
    CHECK_INT(http_format_head(out, (size_t)n + 1, 200, "text/plain", 0, 0), n);  /* 딱 맞음 */

    return TEST_REPORT();
}
