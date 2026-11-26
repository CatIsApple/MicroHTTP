/* http_parser.c — HTTP/1.x 요청 헤더 파서 구현 (RFC 7230 의 최소 범위).
 *
 * 요청 모양:
 *   GET /index.html HTTP/1.1\r\n      ← 요청 라인: 메서드 SP URI SP 버전 CRLF
 *   Host: example.com\r\n             ← 헤더 줄:   이름 ":" 공백* 값 공백* CRLF
 *   Content-Length: 5\r\n
 *   \r\n                              ← 빈 줄 = 헤더 끝
 *   hello                             ← 본문 (여기서는 다루지 않음)
 *
 * 원칙: 애매하면 거절한다. 느슨한 파서는 프록시 앞뒤에서 해석이 달라지는
 * "요청 스머글링" 같은 보안 문제의 원인이 된다. */

#include "../include/http_parser.h" /* http_request, http_header, http_method, 상수, 공개 함수 선언 */
#include <string.h>      /* memset, memcmp, memchr, strchr, strcmp */

/* ---- 문자 분류 ---- */

/* token 문자 (RFC 7230 3.2.6): 메서드와 헤더 이름에 쓸 수 있는 문자 */
static int is_tchar(unsigned char c) /* static = 이 파일 안에서만 쓰는 함수. c 가 token 문자면 1 */
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) /* 영문자·숫자는 항상 허용 */
        return 1;                                                                    /* token 문자 맞음 */
    return c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL; /* 허용 특수문자 목록에 있는지. strchr 는 '\0' 도 "찾음" 처리하므로 먼저 제외 */
}

static unsigned char to_lower(unsigned char c) /* ASCII 대문자 → 소문자 (tolower() 는 로케일 영향을 받아서 직접 구현) */
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c; /* 'A'~'Z' 면 32 를 더해 소문자로, 아니면 그대로 */
}

/* 대소문자 무시 비교. 같으면 1 */
static int ieq(const char *a, const char *b) /* 두 '\0' 종료 문자열을 대소문자 무시로 비교 (헤더 이름 비교용) */
{
    while (*a && *b) {                                         /* 둘 중 하나가 끝날 때까지 한 글자씩 */
        if (to_lower((unsigned char)*a) != to_lower((unsigned char)*b)) /* 소문자로 바꿔 비교, 다르면 */
            return 0;                                          /* 다른 문자열 */
        a++, b++;                                              /* 다음 글자로 */
    }
    return *a == *b; /* 둘 다 '\0' 이어야(같이 끝나야) 같음. "Host" vs "Hostname" 구분 */
}

/* 길이 n 짜리 구간 s 가 소문자 문자열 lower 와 대소문자 무시로 같으면 1 */
static int ieq_n(const char *s, size_t n, const char *lower) /* s 는 '\0' 종료가 아닐 수 있어서 길이를 따로 받음 */
{
    size_t i;                                     /* 비교 위치 */
    for (i = 0; i < n; i++) {                     /* s 의 n 글자를 하나씩 */
        if (lower[i] == '\0' || to_lower((unsigned char)s[i]) != (unsigned char)lower[i]) /* lower 가 먼저 끝났거나 글자가 다르면 */
            return 0;                             /* 다름 */
    }
    return lower[n] == '\0'; /* lower 도 정확히 n 글자에서 끝나야 같음 (lower 가 더 길면 다름) */
}

/* ---- 줄 단위 처리 ---- */

/* [p, limit) 에서 CRLF 를 찾아 '\r' 위치를 돌려주고 *next 에 다음 줄 시작을 넣는다.
 * 단독 CR, 단독 LF 는 줄바꿈으로 인정하지 않음 → NULL (= 400) */
static char *find_eol(char *p, char *limit, char **next) /* eol = end of line. limit: 헤더 영역 끝(넘어가면 안 됨) */
{
    for (; p + 1 < limit; p++) {   /* p 와 p+1 두 바이트를 봐야 하므로 limit-1 까지만 */
        if (*p == '\r') {          /* CR 발견 */
            if (p[1] != '\n')      /* 바로 뒤가 LF 가 아니면 단독 CR */
                return NULL;       /* 잘못된 줄바꿈 */
            *next = p + 2;         /* "\r\n" 다음 = 다음 줄 시작 */
            return p;              /* 이 줄의 끝(= '\r' 위치) */
        }
        if (*p == '\n')            /* CR 없이 LF 만 나옴 = 단독 LF */
            return NULL;           /* 잘못된 줄바꿈 (일부 서버는 허용하지만 우리는 엄격하게) */
    }
    return NULL; /* limit 안에서 CRLF 를 못 찾음 */
}

/* Content-Length 값 → 숫자. 숫자만 허용("+5", " 5", "5,5", "" 모두 거절), 오버플로 검사. 성공 0 */
static int parse_content_length(const char *s, size_t *out) /* strtol 은 "+5", " 5" 를 받아 주므로 직접 구현 */
{
    size_t v = 0;                                  /* 지금까지 읽은 값 */
    if (*s == '\0')                                /* 빈 값 */
        return -1;                                 /* 거절 */
    for (; *s; s++) {                              /* 문자열 끝까지 한 글자씩 */
        if (*s < '0' || *s > '9')                  /* 숫자가 아닌 글자 ('-', '+', ',', 공백 등) */
            return -1;                             /* 거절 */
        size_t d = (size_t)(*s - '0');             /* 글자 '7' → 숫자 7 */
        if (v > ((size_t)-1 - d) / 10) /* v*10 + d 가 size_t 최댓값을 넘는지 "곱하기 전에" 검사 (넘친 뒤엔 못 알아챔) */
            return -1;                             /* 오버플로 → 거절 */
        v = v * 10 + d;                            /* 자릿수 하나 추가 */
    }
    *out = v; /* 결과 저장 */
    return 0; /* 성공 */
}

/* 쉼표로 구분된 목록 값(예: "keep-alive, Upgrade")에 token 이 있는지. token 은 소문자로 줄 것 */
static int has_token(const char *list, const char *token) /* Connection 헤더 값 검사용 */
{
    const char *p = list;                              /* 읽는 위치 */
    while (*p) {                                       /* 문자열 끝까지 항목을 하나씩 */
        while (*p == ' ' || *p == '\t' || *p == ',')   /* 항목 앞의 공백·쉼표 건너뜀 */
            p++;                                       /* 한 칸 앞으로 */
        const char *start = p;                         /* 항목 시작 */
        while (*p && *p != ',')                        /* 다음 쉼표(또는 끝)까지가 한 항목 */
            p++;                                       /* 한 칸 앞으로 */
        const char *end = p;                           /* 항목 끝 (쉼표 위치 또는 '\0') */
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) /* 항목 뒤 공백 제거 */
            end--;                                     /* 끝을 한 칸 당김 */
        if (end > start && ieq_n(start, (size_t)(end - start), token)) /* 빈 항목이 아니고 token 과 같으면 */
            return 1;                                  /* 찾음 */
    }
    return 0; /* 끝까지 없음 */
}

/* ---- 요청 라인 ---- */

/* "메서드 SP URI SP 버전" 한 줄 파싱. line 은 [line, eol) 구간. 성공 0, 실패 시 상태 코드 */
static int parse_request_line(char *line, char *eol, http_request *req) /* eol: 이 줄의 '\r' 위치 */
{
    /* 1) 메서드: 첫 공백까지 */
    char *sp1 = memchr(line, ' ', (size_t)(eol - line)); /* 줄 안에서 첫 공백 찾기 (memchr 은 길이 제한이 있어 안전) */
    if (sp1 == NULL || sp1 == line)                      /* 공백이 없거나 줄이 공백으로 시작(메서드 비어 있음) */
        return 400;                                      /* 잘못된 요청 */
    for (char *p = line; p < sp1; p++)                   /* 메서드 글자 하나씩 */
        if (!is_tchar((unsigned char)*p))                /* token 문자가 아니면 ("G(T" 등) */
            return 400;                                  /* 잘못된 요청 */

    /* 2) URI: 두 번째 공백까지 */
    char *uri = sp1 + 1;                                 /* 첫 공백 다음이 URI 시작 */
    char *sp2 = memchr(uri, ' ', (size_t)(eol - uri));   /* 두 번째 공백 찾기 */
    if (sp2 == NULL || sp2 == uri) /* 공백이 하나뿐(HTTP/0.9 형태)이거나 공백 두 개 연속("GET  /") */
        return 400;                                      /* 잘못된 요청 */
    size_t uri_len = (size_t)(sp2 - uri);                /* URI 길이 */
    if (uri_len > HTTP_MAX_URI_LEN)                      /* 너무 긴 URI */
        return 414;                                      /* 414 URI Too Long */
    for (char *p = uri; p < sp2; p++) {                  /* URI 글자 하나씩 */
        unsigned char c = (unsigned char)*p;             /* 부호 없는 값으로 (0x80 이상을 음수로 보지 않게) */
        if (c <= 0x20 || c == 0x7f) /* 제어 문자·공백·DEL 불가 (SP 는 이미 구분자로 썼음) */
            return 400;                                  /* 잘못된 요청 */
    }
    int is_connect = (size_t)(sp1 - line) == 7 && memcmp(line, "CONNECT", 7) == 0; /* CONNECT 만 대상 형식이 다름 */
    int absolute = uri_len > 7 && ieq_n(uri, 7, "http://");                         /* absolute-form (포워드 프록시용) */
    if (is_connect) {                                    /* CONNECT 는 authority-form("host:port")만 (RFC 7230 5.3.3) */
        if (uri[0] == '/' || absolute)
            return 400;
    } else if (uri[0] != '/' && !absolute) {             /* 그 밖엔 origin-form("/path") 또는 absolute-form 만 */
        return 400;                                      /* 잘못된 요청 ("index.html" 같은 형태) */
    }

    /* 3) 버전: 나머지 전부. 정확히 "HTTP/숫자.숫자" (8바이트) */
    char *ver = sp2 + 1;                                 /* 두 번째 공백 다음이 버전 시작 */
    size_t ver_len = (size_t)(eol - ver);                /* 버전 길이 (줄 끝까지) */
    if (ver_len != 8 || memcmp(ver, "HTTP/", 5) != 0 || /* 길이 8 이 아니거나 "HTTP/" 로 시작 안 함 ("HTTP" 는 대소문자 구분, RFC 7230 2.6) */
        ver[5] < '0' || ver[5] > '9' || ver[6] != '.' || ver[7] < '0' || ver[7] > '9') /* "숫자.숫자" 모양이 아님 */
        return 400; /* 형식 오류. 공백이 하나 더 있는 경우("HTTP/1.1 ")도 길이가 달라져 여기서 걸림 */
    if (ver[5] != '1' || (ver[7] != '0' && ver[7] != '1')) /* 형식은 맞지만 1.0 / 1.1 이 아님 (2.0, 1.2 등) */
        return 505;                                      /* 505 HTTP Version Not Supported */
    req->version_minor = ver[7] - '0';                   /* '0' 또는 '1' → 정수 0 / 1 */

    /* 검증 끝 → 구분자를 '\0' 으로 바꿔 각각을 독립된 문자열로 만든다 */
    *sp1 = '\0';                     /* "GET\0/index.html HTTP/1.1" → 메서드 문자열 끝 */
    *sp2 = '\0';                     /* URI 문자열 끝 */
    *eol = '\0';                     /* 버전 문자열 끝 ('\r' 자리) */
    req->method_str = line;          /* 메서드 원문 = 줄 시작 */
    req->uri = uri;                  /* URI 시작 위치 저장 */
    if (strcmp(line, "GET") == 0) /* 메서드는 대소문자 구분 (RFC 7231 4.1) → "get" 은 OTHER */
        req->method = HTTP_METHOD_GET;    /* GET */
    else if (strcmp(line, "POST") == 0)   /* POST 인지 */
        req->method = HTTP_METHOD_POST;   /* POST */
    else if (strcmp(line, "HEAD") == 0)   /* HEAD 인지 */
        req->method = HTTP_METHOD_HEAD;   /* HEAD */
    else if (is_connect)                  /* CONNECT (포워드 프록시 터널) */
        req->method = HTTP_METHOD_CONNECT;
    else                                  /* 그 밖의 메서드 */
        req->method = HTTP_METHOD_OTHER;  /* 파서는 통과 → 프록시 경로가 아니면 server.c 가 501 */
    return 0; /* 성공 */
}

/* ---- 헤더 줄 ---- */

/* "이름: 값" 한 줄 파싱해서 req->headers 에 추가. 성공 0, 실패 시 상태 코드 */
static int parse_header_line(char *line, char *eol, http_request *req) /* line ~ eol: 헤더 한 줄 (CRLF 제외) */
{
    /* 줄이 공백으로 시작 = obs-fold(이전 헤더 값의 줄 이어쓰기). 폐기된 문법이라 거절 (RFC 7230 3.2.4) */
    if (*line == ' ' || *line == '\t') /* 첫 글자가 SP 또는 HTAB */
        return 400;                    /* 잘못된 요청 */

    char *colon = memchr(line, ':', (size_t)(eol - line)); /* 이름과 값을 가르는 첫 ':' 찾기 (값 안의 ':' 는 상관없음) */
    if (colon == NULL || colon == line)                    /* ':' 가 없거나 이름이 비어 있음(": value") */
        return 400;                                        /* 잘못된 요청 */
    for (char *p = line; p < colon; p++) /* 이름 글자 하나씩. "Host :" 처럼 콜론 앞 공백도 여기서 거절 */
        if (!is_tchar((unsigned char)*p)) /* token 문자가 아니면 */
            return 400;                   /* 잘못된 요청 */

    /* 값: 앞뒤 공백(SP, HTAB) 제거 */
    char *v = colon + 1;                                      /* ':' 다음부터가 값 */
    while (v < eol && (*v == ' ' || *v == '\t'))              /* 값 앞의 공백을 */
        v++;                                                  /* 건너뜀 */
    char *vend = eol;                                         /* 값 끝 (일단 줄 끝) */
    while (vend > v && (vend[-1] == ' ' || vend[-1] == '\t')) /* 값 뒤의 공백을 */
        vend--;                                               /* 잘라냄 */
    for (char *p = v; p < vend; p++) {                        /* 값 글자 하나씩 */
        unsigned char c = (unsigned char)*p;                  /* 부호 없는 값으로 */
        if ((c < 0x20 && c != '\t') || c == 0x7f) /* 제어 문자 불가 (HTAB 은 허용). 0x80 이상(obs-text, 한글 등)은 허용 */
            return 400;                           /* 잘못된 요청 */
    }

    if (req->header_count >= HTTP_MAX_HEADERS) /* 배열이 이미 꽉 참 */
        return 431;                            /* 431 Request Header Fields Too Large */

    *colon = '\0'; /* "Host\0 example.com" → 이름 문자열 끝 */
    *vend = '\0';  /* 값 문자열 끝. vend <= eol 이므로 항상 헤더 영역 안 (본문을 건드리지 않음) */
    req->headers[req->header_count].name = line;  /* 이름 시작 = 줄 시작 */
    req->headers[req->header_count].value = v;    /* 값 시작 (앞 공백 제거된 위치) */
    req->header_count++;                          /* 개수 하나 증가 */
    return 0; /* 성공 */
}

/* ---- 공개 함수 ---- */

int http_parse_request(char *buf, size_t header_len, http_request *req) /* 헤더 전체 파싱 진입점 */
{
    memset(req, 0, sizeof(*req)); /* 결과 구조체 초기화: 포인터는 NULL, 숫자는 0 */

    /* 헤더는 반드시 빈 줄로 끝나야 하고, 중간에 NUL 바이트가 있으면 안 됨
     * (NUL 이 있으면 C 문자열로 다룰 때 뒤쪽이 잘려 보여서 검사를 우회할 수 있음) */
    if (header_len < 4 || memcmp(buf + header_len - 4, "\r\n\r\n", 4) != 0) /* 끝 4바이트가 빈 줄이 아니면 */
        return 400;                                                         /* 불완전한 헤더 */
    if (memchr(buf, '\0', header_len) != NULL)                              /* 헤더 안에 '\0' 바이트가 있으면 */
        return 400;                                                         /* 잘못된 요청 */

    char *limit = buf + header_len; /* 헤더 영역의 끝 (이 뒤는 본문 — 파서는 여기까지만 본다) */
    char *next = NULL;              /* find_eol 이 알려 주는 다음 줄 시작 */

    /* 1) 요청 라인 */
    char *eol = find_eol(buf, limit, &next);  /* 첫 줄의 끝 찾기 */
    if (eol == NULL)                          /* CRLF 가 없거나 잘못된 줄바꿈 */
        return 400;                           /* 잘못된 요청 */
    int rc = parse_request_line(buf, eol, req); /* 메서드·URI·버전 파싱 */
    if (rc != 0)                              /* 실패하면 */
        return rc;                            /* 그 상태 코드를 그대로 반환 */

    /* 2) 헤더 줄들: 빈 줄을 만날 때까지 */
    char *line = next;                        /* 둘째 줄부터 */
    for (;;) {                                /* 빈 줄이나 오류가 나올 때까지 반복 */
        eol = find_eol(line, limit, &next);   /* 이 줄의 끝 찾기 */
        if (eol == NULL)                      /* 잘못된 줄바꿈 */
            return 400;                       /* 잘못된 요청 */
        if (eol == line) /* 줄 시작이 곧 '\r' = 빈 줄 = 헤더 끝 */
            break;                            /* 반복 종료 */
        rc = parse_header_line(line, eol, req); /* "이름: 값" 파싱 */
        if (rc != 0)                          /* 실패하면 */
            return rc;                        /* 그 상태 코드를 반환 */
        line = next;                          /* 다음 줄로 */
    }

    /* 3) 의미 검사: 자주 쓰는 헤더를 구조체 필드로 뽑아낸다 */
    int has_transfer_encoding = 0;            /* Transfer-Encoding 헤더가 있었는지 */
    int conn_close = 0, conn_keep_alive = 0;  /* Connection 헤더에 close / keep-alive 가 있었는지 */
    for (size_t i = 0; i < req->header_count; i++) { /* 모든 헤더를 한 번씩 */
        const char *name = req->headers[i].name;     /* i 번째 헤더 이름 */
        const char *value = req->headers[i].value;   /* i 번째 헤더 값 */

        if (ieq(name, "Host")) {                     /* Host 헤더 (이름은 대소문자 무시) */
            if (req->host != NULL) /* 이미 하나 있었음 = Host 중복 → 400 (RFC 7230 5.4) */
                return 400;                          /* 잘못된 요청 */
            req->host = value;                       /* Host 값 저장 */
        } else if (ieq(name, "Content-Length")) {    /* Content-Length 헤더 */
            size_t cl;                               /* 파싱한 본문 길이 */
            if (parse_content_length(value, &cl) != 0) /* 숫자가 아니거나 오버플로 */
                return 400;                          /* 잘못된 요청 */
            if (req->has_content_length && cl != req->content_length) /* 앞에서 본 값과 다른 중복 → 400 (본문 경계가 모호해짐) */
                return 400;                          /* 잘못된 요청 */
            req->has_content_length = 1;             /* 있음 표시 */
            req->content_length = cl;                /* 값 저장 */
        } else if (ieq(name, "Transfer-Encoding")) { /* Transfer-Encoding 헤더 (chunked 등) */
            has_transfer_encoding = 1;               /* 있음 표시만 (아래에서 501) */
        } else if (ieq(name, "Connection")) { /* Connection 헤더는 여러 줄로 올 수 있어서 매번 검사 */
            if (has_token(value, "close"))           /* 목록에 close 가 있으면 */
                conn_close = 1;                      /* 표시 */
            if (has_token(value, "keep-alive"))      /* 목록에 keep-alive 가 있으면 */
                conn_keep_alive = 1;                 /* 표시 */
        }
    }

    if (req->version_minor == 1 && req->host == NULL) /* HTTP/1.1 은 Host 필수 (RFC 7230 5.4) */
        return 400;                                    /* 잘못된 요청 */
    if (has_transfer_encoding) /* chunked 등은 미지원. CL 과 같이 오면 스머글링 위험도 있음 */
        return 501;            /* 501 Not Implemented */
    /* HTTP_METHOD_OTHER (PUT, DELETE ...) 는 문법만 맞으면 통과시킨다: 프록시 경로는 그대로 백엔드에 전달하고,
     * 정적 파일·echo 경로는 server.c 가 501 로 거절한다 (어느 경로인지는 파서가 모름) */

    /* keep-alive 결정: 1.1 은 기본 유지, 1.0 은 기본 종료. "close" 가 있으면 무조건 종료 */
    if (conn_close)                         /* close 가 최우선 */
        req->keep_alive = 0;                /* 종료 */
    else if (req->version_minor == 1)       /* HTTP/1.1 */
        req->keep_alive = 1;                /* 기본 유지 */
    else                                    /* HTTP/1.0 */
        req->keep_alive = conn_keep_alive;  /* keep-alive 를 명시했을 때만 유지 */

    return 0; /* 파싱 성공 */
}

const char *http_get_header(const http_request *req, const char *name) /* 이름으로 헤더 값 찾기 */
{
    for (size_t i = 0; i < req->header_count; i++) /* 받은 헤더를 순서대로 */
        if (ieq(req->headers[i].name, name))       /* 이름이 같으면 (대소문자 무시) */
            return req->headers[i].value;          /* 첫 번째로 찾은 값 반환 */
    return NULL; /* 없음 */
}
