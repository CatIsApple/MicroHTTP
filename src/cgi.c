/* cgi.c — L5: CGI 실행
 *
 * 레이어 구조에서의 위치
 *   L5 라우터(router.c) 가 /cgi-bin/ 으로 시작하는 요청을 cgi_handle 로 보낸다.
 *   스크립트 출력을 L3 응답(BODY_MEM, owned)으로 바꿔 돌려주고, 보내는 일은 연결 처리(conn.c)가 한다.
 *
 * 구성 (위에서 아래 순서)
 *   1) 환경 변수 만들기: cgi_env, env_add, build_env  (fork 전에 부모가 전부 준비)
 *   2) 실행: make_pipe, run_script  (fork → 자식은 dup2/chdir/execve, 부모는 poll 로 본문 쓰기와 출력 읽기를 함께)
 *   3) 출력 → 응답: find_header_end, ieq, apply_output  (Status / Content-Type / Location 해석)
 *   4) 진입점: cgi_handle  (경로 검증 → 1 → 2 → 3, 실패 종류에 따라 403/404/500/502/504)
 */
#if defined(__linux__)
#define _GNU_SOURCE /* pipe2(O_CLOEXEC): 파이프를 만드는 순간부터 close-on-exec */
#else
#define _XOPEN_SOURCE 700 /* realpath (XSI) */
#endif

#include "cgi.h"
#include "static.h" /* path_is_under */
#include "tcp.h"    /* tcp_now_ms */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define CGI_TIMEOUT_MS 5000           /* 스크립트 실행 전체 제한 시간 */
#define CGI_MAX_OUTPUT (1024 * 1024)  /* 출력 최대 1MB, 넘으면 502 */
#define CGI_ENV_MAX 24                /* 환경 변수 최대 개수 */
#define CGI_ENV_BYTES 8192            /* 환경 변수 문자열을 담는 버퍼 */

/* ---------- 환경 변수 만들기 (fork 전에 부모가 전부 만들어 둔다) ---------- */

typedef struct {
    char *vars[CGI_ENV_MAX + 1]; /* execve 에 넘길 NULL 로 끝나는 배열 */
    size_t n;
    char buf[CGI_ENV_BYTES];
    size_t used;
} cgi_env;

/* "NAME=value" 를 하나 추가. 공간이 없으면 -1 */
static int env_add(cgi_env *e, const char *name, const char *value)
{
    size_t need = strlen(name) + 1 + strlen(value) + 1;
    if (e->n == CGI_ENV_MAX || need > sizeof e->buf - e->used)
        return -1;
    char *s = e->buf + e->used;
    snprintf(s, need, "%s=%s", name, value);
    e->vars[e->n++] = s;
    e->vars[e->n] = NULL;
    e->used += need;
    return 0;
}

/* 요청 정보로 CGI/1.1 환경 변수 목록을 만든다 (메서드, 스크립트 이름, PATH_INFO, 쿼리, 클라이언트 IP,
 * POST 면 CONTENT_LENGTH/TYPE, 그리고 Host, User-Agent). 공간이 모자라면 -1 */
static int build_env(cgi_env *e, const http_request *req, const char *script_name, const char *path_info)
{
    char num[32], addr[64];
    snprintf(num, sizeof num, "%zu", req->content_length);
    /* remote_addr 는 "ip:port" → ip 만 */
    snprintf(addr, sizeof addr, "%s", req->remote_addr ? req->remote_addr : "");
    char *colon = strrchr(addr, ':');
    if (colon)
        *colon = '\0';
    const char *ctype = http_header_get(req, "Content-Type");

    e->n = 0;
    e->used = 0;
    e->vars[0] = NULL;
    int rc = 0;
    rc |= env_add(e, "GATEWAY_INTERFACE", "CGI/1.1");
    rc |= env_add(e, "SERVER_SOFTWARE", "MicroHTTP");
    rc |= env_add(e, "SERVER_PROTOCOL", req->version_minor ? "HTTP/1.1" : "HTTP/1.0");
    rc |= env_add(e, "REQUEST_METHOD", req->method_str);
    rc |= env_add(e, "SCRIPT_NAME", script_name);
    rc |= env_add(e, "PATH_INFO", path_info);
    rc |= env_add(e, "QUERY_STRING", req->query ? req->query : "");
    rc |= env_add(e, "REMOTE_ADDR", addr);
    rc |= env_add(e, "PATH", "/usr/local/bin:/usr/bin:/bin");
    if (req->method == HTTP_POST) {
        rc |= env_add(e, "CONTENT_LENGTH", num);
        if (ctype)
            rc |= env_add(e, "CONTENT_TYPE", ctype);
    }
    if (req->host)
        rc |= env_add(e, "HTTP_HOST", req->host);
    const char *ua = http_header_get(req, "User-Agent");
    if (ua)
        rc |= env_add(e, "HTTP_USER_AGENT", ua);
    return rc;
}

/* ---------- 실행 ---------- */

/* 양쪽 끝이 close-on-exec 인 파이프를 만든다. 다른 CGI 자식에게 이 파이프가 새지 않게 하기 위해서다 */
static int make_pipe(int p[2])
{
#if defined(__linux__)
    return pipe2(p, O_CLOEXEC);
#else
    /* pipe2 가 없는 OS: 다른 스레드가 이 사이에 fork 하면 fd 가 샐 수 있다 (짧은 틈) */
    if (pipe(p) < 0)
        return -1;
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    return 0;
#endif
}

typedef struct {
    char *out;      /* malloc 한 출력 */
    size_t out_len;
    int timed_out;
    int too_large;
    int exit_status; /* waitpid 의 status */
} cgi_result;

/* 자식을 실행하고 stdin 에 본문을 쓰면서 stdout 을 다 읽는다. 시작 자체를 못 하면 -1 */
static int run_script(const char *script, const char *dir, char *const envp[], const char *body,
                      size_t body_len, cgi_result *out)
{
    memset(out, 0, sizeof *out);
    int in[2], outp[2];
    if (make_pipe(in) < 0)
        return -1;
    if (make_pipe(outp) < 0) {
        close(in[0]);
        close(in[1]);
        return -1;
    }
    char *const argv[] = {(char *)script, NULL};

    pid_t pid = fork();
    if (pid < 0) {
        close(in[0]), close(in[1]), close(outp[0]), close(outp[1]);
        return -1;
    }
    if (pid == 0) {
        /* 자식: 스레드가 여러 개인 프로세스에서 fork 하면 자식에는 이 스레드만 남는다.
         * 다른 스레드가 잡고 있던 malloc/printf 잠금이 풀리지 않은 채 복사될 수 있으므로
         * exec 전까지는 async-signal-safe 함수(dup2, chdir, execve, _exit)만 쓴다. */
        if (dup2(in[0], STDIN_FILENO) < 0 || dup2(outp[1], STDOUT_FILENO) < 0)
            _exit(127);
        if (chdir(dir) < 0)
            _exit(127);
        /* dup2 로 만든 0, 1 번은 close-on-exec 가 아니고, 원래 파이프 fd 는 exec 때 닫힌다 */
        execve(script, argv, envp);
        _exit(127);
    }

    /* 부모 */
    close(in[0]);
    close(outp[1]);
    int to_child = in[1], from_child = outp[0];
    /* 스크립트가 본문을 읽지 않고 끝나도 SIGPIPE 로 죽지 않게 (서버는 SIGPIPE 를 무시함) + 논블로킹 */
    fcntl(to_child, F_SETFL, fcntl(to_child, F_GETFL) | O_NONBLOCK);
    if (body_len == 0) {
        close(to_child);
        to_child = -1;
    }

    size_t cap = 16 * 1024, written = 0;
    out->out = malloc(cap);
    long long deadline = tcp_now_ms() + CGI_TIMEOUT_MS;
    /* 쓰기와 읽기를 동시에 poll 한다. 스크립트가 입력을 다 읽기 전에 많이 출력하면
     * 한쪽만 기다리는 방식은 서로 기다리다 멈춘다(교착) */
    while (from_child >= 0 && out->out) {
        long long left = deadline - tcp_now_ms();
        if (left <= 0) {
            out->timed_out = 1;
            break;
        }
        struct pollfd pf[2];
        int n = 0, ri = -1, wi = -1;
        pf[n] = (struct pollfd){.fd = from_child, .events = POLLIN};
        ri = n++;
        if (to_child >= 0) {
            pf[n] = (struct pollfd){.fd = to_child, .events = POLLOUT};
            wi = n++;
        }
        int pr = poll(pf, (nfds_t)n, left > INT_MAX ? INT_MAX : (int)left);
        if (pr < 0 && errno == EINTR)
            continue;
        if (pr < 0)
            break;
        if (wi >= 0 && pf[wi].revents) {
            ssize_t w = write(to_child, body + written, body_len - written);
            if (w > 0)
                written += (size_t)w;
            if ((w < 0 && errno != EAGAIN && errno != EINTR) || written == body_len) {
                close(to_child); /* EOF 를 알려 스크립트가 입력 끝을 알게 함. EPIPE 면 안 읽은 것 */
                to_child = -1;
            }
        }
        if (pf[ri].revents) {
            if (out->out_len == cap) {
                if (cap >= CGI_MAX_OUTPUT) {
                    out->too_large = 1;
                    break;
                }
                char *bigger = realloc(out->out, cap * 2);
                if (!bigger)
                    break;
                out->out = bigger;
                cap *= 2;
            }
            ssize_t r = read(from_child, out->out + out->out_len, cap - out->out_len);
            if (r > 0) {
                out->out_len += (size_t)r;
            } else if (r == 0 || (errno != EINTR && errno != EAGAIN)) {
                close(from_child); /* 출력 끝 */
                from_child = -1;
            }
        }
    }
    if (to_child >= 0)
        close(to_child);
    if (from_child >= 0) { /* 시간 초과나 출력 초과로 중단: 자식을 끝낸다 */
        close(from_child);
        kill(pid, SIGKILL);
    }
    while (waitpid(pid, &out->exit_status, 0) < 0 && errno == EINTR) {
    }
    return 0;
}

/* ---------- 출력 → 응답 ---------- */

/* 줄 끝 "\r\n" 또는 "\n" 모두 허용. 헤더가 끝나는 빈 줄을 찾아 *body 를 정한다 */
static char *find_header_end(char *s, size_t n, char **body)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\n')
            continue;
        if (i + 1 < n && s[i + 1] == '\n') {
            *body = s + i + 2;
            return s + i;
        }
        if (i + 2 < n && s[i + 1] == '\r' && s[i + 2] == '\n') {
            *body = s + i + 3;
            return s + i;
        }
    }
    return NULL;
}

/* 두 문자열이 ASCII 대소문자 무시하고 같은가 (CGI 출력 헤더 이름 비교용) */
static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (((*a >= 'A' && *a <= 'Z') ? *a + 32 : *a) != ((*b >= 'A' && *b <= 'Z') ? *b + 32 : *b))
            return 0;
    return *a == *b;
}

/* CGI 출력의 헤더를 응답 헤더로 옮기고 본문을 BODY_MEM 으로. 출력 형식이 틀리면 -1 */
static int apply_output(cgi_result *cr, http_response *res)
{
    char *body;
    char *end = find_header_end(cr->out, cr->out_len, &body);
    if (!end)
        return -1;
    *end = '\0';
    int status = 0, has_type = 0, has_location = 0;
    for (char *line = cr->out; line && *line;) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\r')
            line[--len] = '\0';
        char *colon = strchr(line, ':');
        if (!colon || colon == line)
            return -1;
        *colon = '\0';
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t')
            value++;
        if (ieq(line, "Status")) { /* "Status: 404 Not Found" */
            status = atoi(value);
            if (status < 100 || status > 599)
                return -1;
        } else if (ieq(line, "Content-Length") || ieq(line, "Connection") || ieq(line, "Date") ||
                   ieq(line, "Server") || ieq(line, "Transfer-Encoding")) {
            /* 서버가 직접 붙이는 헤더는 무시 */
        } else {
            if (ieq(line, "Content-Type"))
                has_type = 1;
            if (ieq(line, "Location"))
                has_location = 1;
            if (http_response_header(res, line, value) < 0)
                return -1;
        }
        line = next;
    }
    if (!has_type && !has_location && !status)
        return -1; /* RFC 3875: 최소한 Content-Type, Location, Status 중 하나는 있어야 한다 */
    res->status = status ? status : (has_location ? 302 : 200);
    size_t body_len = cr->out_len - (size_t)(body - cr->out);
    http_response_body_owned(res, cr->out, body, body_len, NULL); /* 이제 res 가 free 한다 */
    cr->out = NULL;
    return 0;
}

/* CGI 요청 하나 처리: 경로를 스크립트 이름과 PATH_INFO 로 나누고 → realpath 로 cgi_dir 안의
 * 실행 가능한 파일인지 확인 → 환경 변수를 만들어 실행 → 결과에 따라 출력 반영 또는 502/504 */
void cgi_handle(const server_config *cfg, const http_request *req, http_response *res)
{
    if (req->method != HTTP_GET && req->method != HTTP_POST && req->method != HTTP_HEAD) {
        http_response_error(res, 405);
        http_response_header(res, "Allow", "GET, POST, HEAD");
        return;
    }

    /* "/cgi-bin/echo.py/extra" → 스크립트 "echo.py", PATH_INFO "/extra" */
    const char *rest = req->path + strlen(cfg->cgi_prefix);
    size_t name_len = strcspn(rest, "/");
    if (name_len == 0) {
        http_response_error(res, 403); /* /cgi-bin/ 자체 */
        return;
    }
    char script_name[PATH_MAX], joined[PATH_MAX], real[PATH_MAX];
    snprintf(script_name, sizeof script_name, "%s%.*s", cfg->cgi_prefix, (int)name_len, rest);
    if (snprintf(joined, sizeof joined, "%s/%.*s", cfg->cgi_dir, (int)name_len, rest) >= (int)sizeof joined) {
        http_response_error(res, 414);
        return;
    }
    const char *path_info = rest + name_len;

    /* 정적 파일과 같은 방식으로 검증: realpath 결과가 cgi_dir 안에 있어야 한다 */
    struct stat st;
    if (!realpath(joined, real) || stat(real, &st) < 0) {
        http_response_error(res, errno == EACCES ? 403 : 404);
        return;
    }
    if (!path_is_under(cfg->cgi_dir, real) || !S_ISREG(st.st_mode) || access(real, X_OK) < 0) {
        http_response_error(res, 403);
        return;
    }

    cgi_env env;
    if (build_env(&env, req, script_name, path_info) < 0) {
        http_response_error(res, 500);
        return;
    }
    cgi_result cr;
    if (run_script(real, cfg->cgi_dir, env.vars, req->body, req->body_len, &cr) < 0) {
        http_response_error(res, 500);
        return;
    }

    if (cr.timed_out) {
        http_response_error(res, 504);
    } else if (cr.too_large || !cr.out) {
        http_response_error(res, 502);
    } else if (WIFEXITED(cr.exit_status) && WEXITSTATUS(cr.exit_status) == 127 && cr.out_len == 0) {
        http_response_error(res, 502); /* exec 실패 (#! 인터프리터가 없음 등) */
    } else if (!WIFEXITED(cr.exit_status) && cr.out_len == 0) {
        http_response_error(res, 502); /* 시그널로 죽음 */
    } else if (apply_output(&cr, res) < 0) {
        http_response_error(res, 502);
    }
    free(cr.out); /* apply_output 이 넘겼으면 NULL */
}
