/* http_response.c — L3: HTTP 응답 생성 구현. 인터페이스 설명은 include/http_response.h
 *
 * 레이어 구조에서의 위치
 *   핸들러(L4 정적 파일, L5 CGI) 가 http_response 를 채운다 → [L3 이 파일] 이 직렬화 → L1 tcp_send_all 로 전송
 *
 * 구성
 *   - 상태 문구: http_status_text
 *   - 응답 만들기: http_response_init, header / header_copy, body_mem / body_owned / body_file, error
 *   - 정리: drop_body (이전 본문 정리), http_response_free
 *   - 보내기: format_date(Date 헤더) → build_head(상태 줄 + 헤더) → send_file(파일 본문) → http_response_send
 */
#define _POSIX_C_SOURCE 200809L

#include "http_response.h"
#include "tcp.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FILE_CHUNK (64 * 1024) /* 파일 본문을 읽어서 보내는 단위 */

/* 상태 코드에 맞는 이유 문구(reason phrase). 상태 줄 "HTTP/1.1 404 Not Found" 에 쓴다 */
const char *http_status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

/* 구조체를 0 으로 비우고 기본값을 넣는다: 본문 없음, fd -1(파일 없음), keep-alive 켬 */
void http_response_init(http_response *res, int status)
{
    memset(res, 0, sizeof *res);
    res->status = status;
    res->kind = BODY_NONE;
    res->fd = -1;
    res->keep_alive = 1;
}

/* 문자열에 CR 이나 LF 가 있는가 (헤더 값에 있으면 응답이 쪼개질 수 있음) */
static int has_crlf(const char *s) { return strpbrk(s, "\r\n") != NULL; }

/* 헤더 배열 끝에 (이름, 값) 포인터를 추가한다. 값은 복사하지 않는다 */
int http_response_header(http_response *res, const char *name, const char *value)
{
    if (res->n_headers == HTTP_RESPONSE_MAX_HEADERS || has_crlf(name) || has_crlf(value))
        return -1;
    res->headers[res->n_headers].name = name;
    res->headers[res->n_headers].value = value;
    res->n_headers++;
    return 0;
}

/* 값을 res->scratch 에 복사한 뒤 그 복사본으로 헤더를 추가한다. scratch 가 모자라면 -1 */
int http_response_header_copy(http_response *res, const char *name, const char *value)
{
    size_t n = strlen(value) + 1;
    if (n > sizeof res->scratch - res->scratch_used)
        return -1;
    char *copy = res->scratch + res->scratch_used;
    memcpy(copy, value, n);
    if (http_response_header(res, name, copy) < 0)
        return -1;
    res->scratch_used += n;
    return 0;
}

/* 이전에 정한 본문을 정리한다 (파일 닫기, owned 해제) */
static void drop_body(http_response *res)
{
    if (res->fd >= 0)
        close(res->fd);
    free(res->owned);
    res->fd = -1;
    res->owned = NULL;
    res->kind = BODY_NONE;
    res->mem = NULL;
    res->mem_len = 0;
    res->file_len = 0;
}

/* 이전 본문을 정리하고 메모리 [p, p+n) 를 본문으로 정한다 (복사하지 않음) */
void http_response_body_mem(http_response *res, const void *p, size_t n, const char *ctype)
{
    drop_body(res);
    res->kind = BODY_MEM;
    res->mem = p;
    res->mem_len = n;
    if (ctype)
        http_response_header(res, "Content-Type", ctype);
}

/* body_mem 과 같지만 base 의 소유권도 넘겨받아 http_response_free 에서 free 한다 */
void http_response_body_owned(http_response *res, void *base, const void *p, size_t n, const char *ctype)
{
    http_response_body_mem(res, p, n, ctype);
    res->owned = base;
}

/* 이전 본문을 정리하고 열린 파일 fd 를 본문으로 정한다. fd 소유권은 res 로 넘어온다 */
void http_response_body_file(http_response *res, int fd, off_t len, const char *ctype)
{
    drop_body(res);
    res->kind = BODY_FILE;
    res->fd = fd;
    res->file_len = len;
    if (ctype)
        http_response_header(res, "Content-Type", ctype);
}

/* 응답을 오류 응답으로 바꾼다: 본문·헤더를 비우고 "<h1>404 Not Found</h1>" 같은 HTML 을 error_page 에 만든다 */
void http_response_error(http_response *res, int status)
{
    drop_body(res);
    res->n_headers = 0; /* 성공 응답용으로 붙였던 헤더는 버린다 */
    res->scratch_used = 0;
    res->status = status;
    const char *text = http_status_text(status);
    int n = snprintf(res->error_page, sizeof res->error_page,
                     "<!DOCTYPE html>\n<html><head><title>%d %s</title></head>\n"
                     "<body><h1>%d %s</h1><hr><p>MicroHTTP</p></body></html>\n",
                     status, text, status, text);
    http_response_body_mem(res, res->error_page, (size_t)n, "text/html; charset=utf-8");
}

/* 응답이 가진 자원(파일 fd, owned 메모리)을 돌려준다 */
void http_response_free(http_response *res) { drop_body(res); }

/* RFC 9110 형식의 현재 시각: "Tue, 24 Nov 2026 05:00:00 GMT" */
static void format_date(char *out, size_t cap)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    /* strftime 의 %a, %b 는 locale 을 따르므로 영어 이름을 직접 쓴다 */
    static const char *const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    snprintf(out, cap, "%s, %02d %s %04d %02d:%02d:%02d GMT", days[tm.tm_wday], tm.tm_mday,
             months[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* 상태 줄 + 헤더 + 빈 줄을 out 에 쓴다. 반환: 길이, 넘치면 -1 */
static int build_head(const http_response *res, char *out, size_t cap)
{
    char date[64];
    format_date(date, sizeof date);
    unsigned long long length = res->kind == BODY_MEM    ? (unsigned long long)res->mem_len
                                : res->kind == BODY_FILE ? (unsigned long long)res->file_len
                                                         : 0;
    int n = snprintf(out, cap,
                     "HTTP/1.1 %d %s\r\n"
                     "Date: %s\r\n"
                     "Server: MicroHTTP\r\n"
                     "Content-Length: %llu\r\n"
                     "Connection: %s\r\n",
                     res->status, http_status_text(res->status), date, length,
                     res->keep_alive ? "keep-alive" : "close");
    if (n < 0 || (size_t)n >= cap)
        return -1;
    size_t used = (size_t)n;
    for (size_t i = 0; i < res->n_headers; i++) {
        n = snprintf(out + used, cap - used, "%s: %s\r\n", res->headers[i].name, res->headers[i].value);
        if (n < 0 || (size_t)n >= cap - used)
            return -1;
        used += (size_t)n;
    }
    if (cap - used < 3)
        return -1;
    memcpy(out + used, "\r\n", 3);
    return (int)(used + 2);
}

/* 파일을 처음부터 len 바이트 읽어 보낸다 */
static int send_file(int sock, int fd, off_t len, long long deadline_ms)
{
    char *chunk = malloc(FILE_CHUNK);
    if (!chunk)
        return -1;
    if (lseek(fd, 0, SEEK_SET) < 0) {
        free(chunk);
        return -1;
    }
    int rc = 0;
    while (len > 0) {
        size_t want = len < FILE_CHUNK ? (size_t)len : FILE_CHUNK;
        ssize_t n = read(fd, chunk, want);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) { /* 오류, 또는 보내는 도중 파일이 짧아짐: 이미 Content-Length 를 보냈으니 끊는 수밖에 없다 */
            rc = -1;
            break;
        }
        if (tcp_send_all(sock, chunk, (size_t)n, deadline_ms) < 0) {
            rc = -1;
            break;
        }
        len -= n;
    }
    free(chunk);
    return rc;
}

/* 헤더를 만들어 보내고 이어서 본문을 보낸다.
 * 헤더가 head 버퍼를 넘치면 500 으로 바꿔 보내고, 작은 메모리 본문은 헤더와 한 번에 보낸다 */
int http_response_send(int fd, http_response *res, int head_only, long long deadline_ms)
{
    char head[4096];
    int n = build_head(res, head, sizeof head);
    if (n < 0) { /* 헤더가 너무 길면 대신 500 을 보낸다 */
        http_response_error(res, 500);
        res->keep_alive = 0;
        n = build_head(res, head, sizeof head);
        if (n < 0)
            return -1;
    }

    /* 작은 메모리 본문은 헤더와 이어 붙여 한 번에 보낸다 (작은 패킷 두 개 대신 하나) */
    if (!head_only && res->kind == BODY_MEM && res->mem_len <= sizeof head - (size_t)n) {
        memcpy(head + n, res->mem, res->mem_len);
        return tcp_send_all(fd, head, (size_t)n + res->mem_len, deadline_ms);
    }
    if (tcp_send_all(fd, head, (size_t)n, deadline_ms) < 0)
        return -1;
    if (head_only)
        return 0;
    if (res->kind == BODY_MEM)
        return tcp_send_all(fd, res->mem, res->mem_len, deadline_ms);
    if (res->kind == BODY_FILE)
        return send_file(fd, res->fd, res->file_len, deadline_ms);
    return 0;
}
