/*
 * auth.c — Cookie 기반 로그인 상태 관리
 *
 * 흐름
 *   로그인:   비밀번호 확인 → session_create() → Set-Cookie: sid=<64자 난수>
 *   이후 요청: Cookie 헤더의 sid → session_lookup() → 복사본(SessionInfo)으로 응답
 *   로그아웃: session_destroy() → 같은 sid로 다시 와도 401
 *
 * 연결(fd)은 식별에 쓰지 않습니다. AUTH_FD_DEMO로 빌드하면 일부러 fd로 식별하는 잘못된 방식을 씁니다
 * (STEP 1 실험: `make build/server_fd_demo`).
 */
#include "auth.h"
#include "net.h"
#include "session.h"

#include <sodium.h>
#include <stdio.h>
#include <string.h>

enum { MAX_USERS = 16, FORM_FIELD_MAX = 128 };

typedef struct {
    char name[SESSION_USER_MAX];
    char hash[crypto_pwhash_STRBYTES];
} User;

/* 시작할 때 한 번 채우고 이후 읽기만 하므로 락이 필요 없습니다. */
static User users[MAX_USERS];
static int user_count;

/*
 * Cookie 속성
 *   HttpOnly     : 페이지의 JavaScript(document.cookie)가 읽지 못함 → XSS로 ID를 훔치기 어려움
 *   SameSite=Lax : 다른 사이트에서 시작된 POST 등에는 Cookie를 보내지 않음 → CSRF 위험 감소
 *   Path=/       : 사이트 전체에서 사용
 *   Secure 없음  : 이 서버는 아직 HTTP만 지원합니다. Secure를 붙이면 HTTP에서는 브라우저가 Cookie를 보내지 않습니다.
 *                  (STEP 5: TLS를 붙일 때 추가 — README 한계 절)
 */
#define COOKIE_ATTRIBUTES "Path=/; HttpOnly; SameSite=Lax"

int auth_init(const char *users_path)
{
    if (sodium_init() < 0) {
        fprintf(stderr, "libsodium init failed\n");
        return -1;
    }
    if (!users_path) return 0;
    FILE *file = fopen(users_path, "r");
    if (!file) {
        perror(users_path);
        return -1;
    }
    char line[512];
    while (fgets(line, sizeof(line), file) && user_count < MAX_USERS) {
        char *colon = strchr(line, ':');
        if (!colon || line[0] == '#') continue;
        *colon = '\0';
        char *hash = colon + 1;
        hash[strcspn(hash, "\r\n")] = '\0';
        if (strlen(line) >= SESSION_USER_MAX || strlen(hash) >= crypto_pwhash_STRBYTES) continue;
        strcpy(users[user_count].name, line);
        strcpy(users[user_count].hash, hash);
        ++user_count;
    }
    fclose(file);
    fprintf(stderr, "auth: %d user(s) loaded\n", user_count);
    return 0;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * application/x-www-form-urlencoded 본문 "username=alice&password=p%40ss"에서 name의 값을 꺼냅니다.
 * '+'는 공백, %XX는 바이트로 풉니다. 찾으면 1, 없거나 너무 길거나 잘못된 인코딩이면 0.
 */
static int form_value(const unsigned char *body, size_t length, const char *name,
                      char *out, size_t capacity)
{
    size_t name_length = strlen(name);
    size_t i = 0;
    while (i < length) {
        size_t start = i;
        while (i < length && body[i] != '&') ++i;           /* [start, i) = "key=value" */
        if (i - start > name_length && body[start + name_length] == '=' &&
            memcmp(body + start, name, name_length) == 0) {
            size_t used = 0;
            for (size_t k = start + name_length + 1; k < i; ++k) {
                int c = body[k];
                if (c == '+') c = ' ';
                else if (c == '%') {
                    int high = k + 2 < i ? hex_digit((char)body[k + 1]) : -1;
                    int low = high < 0 ? -1 : hex_digit((char)body[k + 2]);
                    if (low < 0) return 0;
                    c = high * 16 + low;
                    k += 2;
                }
                if (c == 0 || used + 1 >= capacity) return 0;
                out[used++] = (char)c;
            }
            out[used] = '\0';
            return 1;
        }
        ++i; /* '&' 건너뛰기 */
    }
    return 0;
}

/* Cookie 헤더 "theme=dark; sid=abc"에서 sid 값을 꺼냅니다. 찾으면 1. */
static int cookie_session_id(const HttpRequest *request, char id[SESSION_ID_LENGTH + 1])
{
    const unsigned char *value;
    size_t length;
    if (!http_find_header(request, "Cookie", &value, &length)) return 0;
    size_t i = 0;
    while (i < length) {
        while (i < length && (value[i] == ' ' || value[i] == ';')) ++i;
        size_t start = i;
        while (i < length && value[i] != ';') ++i;
        /* [start, i) = "name=value" */
        if (i - start == 4 + SESSION_ID_LENGTH && memcmp(value + start, "sid=", 4) == 0) {
            memcpy(id, value + start + 4, SESSION_ID_LENGTH);
            id[SESSION_ID_LENGTH] = '\0';
            return 1;
        }
    }
    return 0;
}

/* 헤더 + 짧은 텍스트 본문을 보냅니다. 인증 응답은 사람마다 다르므로 항상 Cache-Control: no-store. */
static int send_auth_response(int fd, int status, int keep_alive, int omit_body,
                              const char *allow, const char *extra, const char *body)
{
    char headers[512];
    int written = snprintf(headers, sizeof(headers), "Cache-Control: no-store\r\n%s", extra ? extra : "");
    if (written < 0 || (size_t)written >= sizeof(headers)) return -1;
    char header[1024];
    size_t body_length = strlen(body);
    int header_length = http_response_header(header, sizeof(header), status, "text/plain; charset=utf-8",
                                             body_length, keep_alive, allow, headers);
    if (header_length < 0 || send_all(fd, header, (size_t)header_length) == -1) return -1;
    if (!omit_body && send_all(fd, body, body_length) == -1) return -1;
    return status;
}

/* 이름과 비밀번호를 확인합니다. 맞으면 1. */
static int check_password(const char *name, const char *password)
{
    for (int i = 0; i < user_count; ++i)
        if (strcmp(users[i].name, name) == 0)
            return crypto_pwhash_str_verify(users[i].hash, password, strlen(password)) == 0;
    return 0;
}

#ifdef AUTH_FD_DEMO
/*
 * ── 잘못된 설계 데모: 연결(fd) 번호로 사용자를 식별 ──
 * 로그인한 연결의 fd에 사용자 이름을 적어 둡니다. 연결이 닫혀도 지우지 않습니다.
 * 커널은 닫힌 fd 번호를 다음 연결에 다시 주므로, 다른 클라이언트가 그 번호를 받으면 로그인한 것으로 보입니다.
 * (반대로 닫을 때 지우면, 같은 사람이 새 연결로 오는 순간 로그인이 풀립니다 — 어느 쪽이든 틀림)
 * 실험 전용이며 락도 없습니다.
 */
static char fd_user[1024][SESSION_USER_MAX];

static int demo_handle(int fd, const HttpRequest *request, const char *path, int keep_alive)
{
    if (fd < 0 || fd >= 1024) return 0;
    if (strcmp(path, "/login") == 0 && strcmp(request->method, "POST") == 0) {
        char name[FORM_FIELD_MAX], password[FORM_FIELD_MAX];
        if (form_value(request->body, request->body_length, "username", name, sizeof(name)) &&
            form_value(request->body, request->body_length, "password", password, sizeof(password)) &&
            check_password(name, password)) {
            snprintf(fd_user[fd], sizeof(fd_user[fd]), "%s", name);
            return send_auth_response(fd, 200, keep_alive, 0, NULL, NULL, "logged in (fd demo)\n");
        }
        return send_auth_response(fd, 401, keep_alive, 0, NULL, NULL, "invalid username or password\n");
    }
    if (strcmp(path, "/me") == 0) {
        char body[128];
        if (!fd_user[fd][0]) return send_auth_response(fd, 401, keep_alive, 0, NULL, NULL, "not logged in\n");
        snprintf(body, sizeof(body), "user: %s (identified by fd %d)\n", fd_user[fd], fd);
        return send_auth_response(fd, 200, keep_alive, 0, NULL, NULL, body);
    }
    return 0;
}
#endif

int auth_handle_request(int client_fd, const HttpRequest *request, int keep_alive)
{
    /* 쿼리를 뗀 경로 */
    char path[64];
    size_t path_length = strcspn(request->target, "?");
    if (path_length >= sizeof(path)) return 0;
    memcpy(path, request->target, path_length);
    path[path_length] = '\0';
    if (strcmp(path, "/login") && strcmp(path, "/logout") && strcmp(path, "/me")) return 0;

#ifdef AUTH_FD_DEMO
    return demo_handle(client_fd, request, path, keep_alive);
#endif

    int is_head = strcmp(request->method, "HEAD") == 0;
    char id[SESSION_ID_LENGTH + 1];
    int has_cookie = cookie_session_id(request, id);

    if (strcmp(path, "/me") == 0) {
        if (strcmp(request->method, "GET") && !is_head)
            return send_auth_response(client_fd, 405, keep_alive, 0, "GET, HEAD", NULL, "405 Method Not Allowed\n");
        SessionInfo info;
        if (!has_cookie || !session_lookup(id, session_now(), &info))
            return send_auth_response(client_fd, 401, keep_alive, is_head, NULL, NULL, "not logged in\n");
        /* 여기부터는 락 밖. info는 복사본이라 다른 스레드가 이 Session을 지워도 안전합니다. */
        char body[128];
        snprintf(body, sizeof(body), "user: %s\n", info.user);
        return send_auth_response(client_fd, 200, keep_alive, is_head, NULL, NULL, body);
    }

    if (strcmp(request->method, "POST"))
        return send_auth_response(client_fd, 405, keep_alive, 0, "POST", NULL, "405 Method Not Allowed\n");

    if (strcmp(path, "/logout") == 0) {
        if (has_cookie) session_destroy(id); /* 서버 측 무효화: 이 ID는 이제 어디서 와도 401 */
        return send_auth_response(client_fd, 200, keep_alive, 0, NULL,
                                  "Set-Cookie: sid=; Max-Age=0; " COOKIE_ATTRIBUTES "\r\n", "logged out\n");
    }

    /* /login */
    char name[FORM_FIELD_MAX], password[FORM_FIELD_MAX];
    int ok = form_value(request->body, request->body_length, "username", name, sizeof(name)) &&
             form_value(request->body, request->body_length, "password", password, sizeof(password)) &&
             check_password(name, password);
    sodium_memzero(password, sizeof(password));
    if (!ok)
        return send_auth_response(client_fd, 401, keep_alive, 0, NULL, NULL, "invalid username or password\n");

    /*
     * Session 고정(fixation) 방지: 로그인 전에 쓰던 ID가 있으면 버리고 항상 새 ID를 발급합니다.
     * 공격자가 미리 심어 둔 ID가 로그인 후에도 살아 있으면 그 ID로 피해자 계정에 들어올 수 있습니다.
     */
    if (has_cookie) session_destroy(id);
    char new_id[SESSION_ID_LENGTH + 1];
    if (session_create(name, session_now(), new_id) == -1)
        return send_auth_response(client_fd, 500, keep_alive, 0, NULL, NULL, "500 Internal Server Error\n");
    char extra[256];
    snprintf(extra, sizeof(extra), "Location: /me\r\nSet-Cookie: sid=%s; " COOKIE_ATTRIBUTES "\r\n", new_id);
    sodium_memzero(new_id, sizeof(new_id));
    /* 로그에는 Session ID를 남기지 않습니다(server.c는 경로와 상태 코드만 기록). */
    return send_auth_response(client_fd, 303, keep_alive, 0, NULL, extra, "logged in\n");
}
