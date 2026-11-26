/* static_file.c — URI → 파일 경로 해석과 파일 찾기 (정적 파일 서빙).
 *
 * 요청 하나의 비용: openat 1 + fstat 1 (+ 보낸 뒤 close 1). 하위 디렉터리 한 단계마다 openat + close 가 하나씩 더
 * (심볼릭 링크 차단 때문, open_nofollow 참고). 경로 해석은 4KB 스택 버퍼에서 끝나고 할당이 없다.
 * 실제 본문 전송은 main 이 http_send_response(use_file) → tcp_sendfile_all 로 한다. */

#define _POSIX_C_SOURCE 200809L /* openat, O_CLOEXEC, O_DIRECTORY (POSIX 2008) */

#include "static_file.h"   /* 이 파일이 구현할 함수들의 선언 */
#include "http_response.h" /* http_format_date, http_parse_date (Last-Modified / If-Modified-Since) */
#include "mime.h"          /* mime_type_for */

#include <errno.h>    /* errno, ENOENT, EACCES ... */
#include <fcntl.h>    /* openat, open, O_* */
#include <stdio.h>    /* snprintf */
#include <string.h>   /* memcpy, memcmp, strlen, strstr, strcspn */
#include <sys/stat.h> /* fstat, struct stat, S_ISREG, S_ISDIR */
#include <time.h>     /* time */
#include <unistd.h>   /* close */

int static_open_root(const char *dir)
{
    /* O_DIRECTORY: 디렉터리가 아니면 실패 (오타로 파일을 루트로 주는 실수 방지)
     * O_CLOEXEC  : 나중에 CGI 등으로 exec 할 때 자식에게 새지 않게 */
    return open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

/* ---- 1. 경로 해석 (순수 함수) ---- */

static int hexval(char c) /* 16진수 글자 → 0~15, 아니면 -1 */
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* URI 에서 경로 부분의 시작. origin-form("/a") 은 그대로,
 * absolute-form("http://host:8080/a?x") 은 authority 를 건너뛴 "/a?x", 경로가 비었으면 "/". 형식이 아니면 NULL */
static const char *path_part(const char *uri)
{
    if (uri[0] == '/')                          /* 보통의 요청 */
        return uri;
    const char *sep = strstr(uri, "://");       /* scheme 끝 */
    if (sep == NULL || sep == uri)
        return NULL;
    for (const char *q = uri; q < sep; q++)     /* scheme 은 영문자·숫자·"+-." 만 (RFC 3986 3.1) */
        if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') ||
              *q == '+' || *q == '-' || *q == '.'))
            return NULL;
    const char *after = sep + 3 + strcspn(sep + 3, "/?#"); /* authority(host:port) 끝 */
    return *after == '/' ? after : "/";         /* "http://host" / "http://host?x" → 루트 */
}

int static_resolve(const char *uri, char *out, size_t cap, int *dir_request)
{
    if (dir_request)
        *dir_request = 0;
    const char *p = path_part(uri);
    if (p == NULL)                              /* '/' 로도, scheme:// 로도 시작하지 않음 ("*", "abc") */
        return 400;
    if (cap == 0)
        return 414;

    /* (1) 쿼리·조각 앞까지 %XX 디코딩. 검사는 디코딩 "뒤" 에 해야 "%2e%2e" 같은 우회가 안 통한다.
     *     "%2F" 도 '/' 로 풀려 구분자가 된다 (nginx 와 같은 동작). */
    char dec[STATIC_PATH_MAX];
    size_t dl = 0;
    for (; *p != '\0' && *p != '?' && *p != '#'; p++) {
        char c = *p;
        if (c == '%') {
            int hi = hexval(p[1]);
            int lo = hi < 0 ? -1 : hexval(p[2]); /* p[1] 이 '\0' 이면 p[2] 는 읽지 않음 */
            if (lo < 0)
                return 400;                     /* "%", "%4", "%zz" */
            c = (char)(hi * 16 + lo);
            if (c == '\0')
                return 400;                     /* NUL 은 C 문자열을 거기서 끊어 버림 → "a.php%00.png" 류 우회 차단 */
            p += 2;
        }
        if (dl + 1 >= sizeof(dec))
            return 414;
        dec[dl++] = c;
    }

    /* (2) '/' 로 나눈 세그먼트를 하나씩 out 에 쌓는다.
     *     빈 세그먼트("//") 와 "." 는 건너뛰고, ".." 는 앞 세그먼트를 지운다. */
    size_t n = 0;                               /* out 에 쓴 길이 */
    int dir = 1;                                /* 마지막 세그먼트가 디렉터리를 뜻하는지 ("/", "/a/", "/a/.") */
    size_t i = 0;
    while (i < dl) {                            /* dec[i] 는 항상 '/' (경로는 '/' 로 시작) */
        size_t start = i + 1, end = start;
        while (end < dl && dec[end] != '/')
            end++;
        const char *seg = dec + start;
        size_t len = end - start;
        i = end;

        if (len == 0 || (len == 1 && seg[0] == '.')) { /* "//" 또는 "/./" */
            dir = 1;
            continue;
        }
        if (len == 2 && seg[0] == '.' && seg[1] == '.') { /* ".." */
            if (n == 0)
                return 403;                     /* 루트보다 위로 → 디렉터리 탈출 시도 */
            while (n > 0 && out[n - 1] != '/')  /* 마지막 세그먼트 지우기 */
                n--;
            if (n > 0)
                n--;                            /* 그 앞의 '/' 도 */
            dir = 1;
            continue;
        }
        if (seg[0] == '.' && !(len == 11 && memcmp(seg, ".well-known", 11) == 0))
            return 403;                         /* 숨김 파일·디렉터리 (.git, .env, .htpasswd ...) 는 내보내지 않음 */

        if (n + (n > 0) + len + 1 > cap)        /* '/' + 세그먼트 + '\0' 자리 */
            return 414;
        if (n > 0)
            out[n++] = '/';
        memcpy(out + n, seg, len);
        n += len;
        dir = 0;
    }

    if (dir) {                                  /* 디렉터리 요청 → index.html */
        size_t il = sizeof(STATIC_INDEX) - 1;
        if (n + (n > 0) + il + 1 > cap)
            return 414;
        if (n > 0)
            out[n++] = '/';
        memcpy(out + n, STATIC_INDEX, il);
        n += il;
        if (dir_request)
            *dir_request = 1;
    }
    out[n] = '\0';
    return 0;
}

/* ---- 2. 파일 찾기 ---- */

/* rel("a/b/c.html") 을 문서 루트에서 한 단계씩 O_NOFOLLOW 로 열어 내려간다.
 * 경로 어디에든 심볼릭 링크가 있으면 ELOOP 로 실패 → "www/x -> /etc/passwd" 같은 링크로 루트 밖을 읽을 수 없다.
 * (static_resolve 의 ".." 차단은 문자열만 보므로 링크는 못 막는다. nginx 의 disable_symlinks on 과 같은 방식)
 * openat 한 번에 "a/b/c" 를 주면 커널이 중간 링크를 따라가 버리고, O_NOFOLLOW 는 마지막 이름에만 걸리기 때문에 나눠 연다.
 * 중간 디렉터리는 읽기 권한(r)이 있어야 열린다 (실행 권한만 있는 디렉터리는 403).
 * 깊이 0 ("index.html") 이면 openat 한 번으로 끝난다. 실패 시 -1, errno 는 실패한 openat 의 것. */
static int open_nofollow(int root_fd, const char *rel, int flags)
{
    char buf[STATIC_PATH_MAX];                  /* '/' 자리에 '\0' 을 써 가며 이름을 하나씩 끊으려고 복사 */
    size_t len = strlen(rel);
    if (len >= sizeof(buf)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, rel, len + 1);

    int dfd = root_fd;                          /* 지금 있는 디렉터리 */
    char *name = buf;
    char *slash;
    while ((slash = strchr(name, '/')) != NULL) { /* 마지막 이름 전까지는 디렉터리 */
        *slash = '\0';
        int next = openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int err = errno;
        struct stat lst;                        /* 리눅스는 디렉터리 링크에 O_DIRECTORY|O_NOFOLLOW 면 ELOOP 대신 ENOTDIR */
        if (next < 0 && err == ENOTDIR && fstatat(dfd, name, &lst, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(lst.st_mode))
            err = ELOOP;                        /* → "진짜 파일이 중간에 있음(404)" 과 구분해 링크(403) 로 (실패 경로에서만 확인) */
        if (dfd != root_fd)
            close(dfd);                         /* 한 단계 내려갔으니 위 디렉터리는 닫음 (루트는 유지) */
        if (next < 0) {
            errno = err;
            return -1;
        }
        dfd = next;
        name = slash + 1;
    }
    int fd = openat(dfd, name, flags | O_NOFOLLOW);
    int err = errno;
    if (dfd != root_fd)
        close(dfd);
    errno = err;
    return fd;
}

static int status_from_errno(int err) /* openat 실패 → 상태 코드 */
{
    switch (err) {
    case ENOENT:       /* 없음 */
    case ENOTDIR:      /* 중간 경로가 파일 ("/a.txt/b") */
    case ENAMETOOLONG: /* 이름이 파일시스템 한도 초과 */
        return 404;
    case ELOOP:        /* 경로에 심볼릭 링크 (O_NOFOLLOW) → 따라가지 않음 */
    case EACCES:       /* 읽기 권한 없음 */
    case EPERM:
        return 403;
    default:           /* EMFILE(fd 고갈), EIO 등 서버 사정 */
        return 500;
    }
}

void static_lookup(int root_fd, const char *uri, const char *if_modified_since, static_result *out)
{
    out->status = 500;
    out->fd = -1;
    out->size = 0;
    out->content_type = NULL;
    out->headers[0] = '\0';

    char rel[STATIC_PATH_MAX];
    int dir_request;
    int rc = static_resolve(uri, rel, sizeof(rel), &dir_request);
    if (rc != 0) {
        out->status = rc;
        return;
    }

    /* O_NONBLOCK: 루트 안에 FIFO 가 있어도 open 에서 영원히 멈추지 않게 (일반 파일에는 영향 없음)
     * O_NOCTTY  : 터미널 장치를 열어도 제어 터미널이 되지 않게
     * open_nofollow: 경로의 심볼릭 링크를 따라가지 않음 (루트 밖 탈출 차단) */
    int fd = open_nofollow(root_fd, rel, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) {
        out->status = status_from_errno(errno);
        return;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {                   /* 경로가 아니라 연 fd 로 확인 → 확인과 사용 사이에 파일이 바뀌어도 안전 */
        close(fd);
        return;                                 /* 500 */
    }

    if (S_ISDIR(st.st_mode)) {                  /* 디렉터리 */
        close(fd);
        if (dir_request) {                      /* ".../index.html" 이 디렉터리 (이상한 구성) → 리다이렉트하면 무한 반복 */
            out->status = 403;
            return;
        }
        /* "/docs" → 301 "/docs/". 그래야 index.html 안의 상대 링크("a.css")가 "/docs/a.css" 로 풀린다.
         * 받은 URI 의 경로(인코딩 그대로) 뒤에 '/' 를 붙이고 쿼리는 유지.
         * 앞의 '/' 들은 하나로 줄인다: "//evil.com/" 은 브라우저가 "다른 호스트" 로 읽어서 (스킴 상대 URL)
         * "GET //evil.com" 이 외부 사이트로 보내는 열린 리다이렉트가 된다. '\' 도 브라우저가 '/' 로 취급하므로 같이 건너뜀. */
        const char *path = path_part(uri);
        while (*path == '/' || *path == '\\')
            path++;
        int plen = (int)strcspn(path, "?#");
        int qlen = path[plen] == '?' ? (int)strcspn(path + plen, "#") : 0;
        int w = snprintf(out->headers, sizeof(out->headers), "Location: /%.*s/%.*s\r\n",
                         plen, path, qlen, path + plen);
        if (w < 0 || (size_t)w >= sizeof(out->headers)) {
            out->headers[0] = '\0';
            return;                             /* 500 (URI 상한 때문에 사실상 없음) */
        }
        out->status = 301;
        return;
    }
    if (!S_ISREG(st.st_mode)) {                 /* FIFO, 소켓, 장치 파일 → 내보내지 않음 */
        close(fd);
        out->status = 403;
        return;
    }

    char lm[32];                                /* Last-Modified 값 */
    if (http_format_date(lm, sizeof(lm), st.st_mtime) < 0) {
        close(fd);
        return;                                 /* 500 */
    }
    snprintf(out->headers, sizeof(out->headers), "Last-Modified: %s\r\n", lm);

    /* 조건부 GET (RFC 7232 3.3): 브라우저가 가진 사본의 시각 이후로 안 바뀌었으면 304 → 본문을 다시 안 보냄.
     * 해석 못 하는 날짜, 미래 시각(서버 시계보다 뒤)은 무시하고 200. */
    if (if_modified_since != NULL) {
        time_t ims;
        if (http_parse_date(if_modified_since, &ims) == 0 && ims <= time(NULL) && st.st_mtime <= ims) {
            close(fd);
            out->status = 304;
            return;
        }
    }

    out->status = 200;
    out->fd = fd;
    out->size = (size_t)st.st_size;
    out->content_type = mime_type_for(rel);
}
