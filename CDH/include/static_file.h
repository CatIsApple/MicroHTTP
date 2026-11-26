/* static_file.h — 정적 파일 서빙의 "목차"(선언부). 구현은 src/static_file.c.
 *
 * 두 층:
 *   1. static_resolve : URI → 문서 루트 기준 상대 경로. 소켓·파일과 무관한 순수 함수 (단위 테스트 쉬움).
 *                       퍼센트 디코딩, "." / ".." 정리, 루트 탈출·숨김 파일 차단을 여기서 한다.
 *   2. static_lookup  : 그 경로를 문서 루트 fd 기준으로 openat → fstat 해서 응답할 내용을 정한다.
 *                       파일 내용은 읽지 않는다 — 열린 fd 를 넘기면 http_send_response 가 sendfile 로 보낸다.
 *
 * 경로를 문자열로 이어 붙여(root + "/" + path) open 하지 않고, 시작할 때 한 번 연 문서 루트 디렉터리 fd 에
 * openat 한다 → 요청마다 루트 경로를 다시 해석하지 않고, realpath 같은 비싼 호출도 없다.
 * 루트 밖으로 나가는 것은 두 겹으로 막는다:
 *   - static_resolve 가 ".." 를 문자열 단계에서 차단
 *   - static_lookup 이 경로를 한 단계씩 O_NOFOLLOW 로 열어 심볼릭 링크를 거부 (403).
 *     "www/x -> /etc/passwd" 같은 링크로 루트 밖 파일이 나가지 않게. (nginx 의 disable_symlinks on 과 같음)
 *     문서 루트 자체(-r 로 준 경로)가 링크인 것은 괜찮다 — 시작할 때 한 번 따라가서 연다. */

#ifndef STATIC_FILE_H /* 인클루드 가드 */
#define STATIC_FILE_H

#include <stddef.h> /* size_t */

#define STATIC_INDEX "index.html"   /* 디렉터리 요청("/", "/docs/") 에 붙이는 파일 이름 */
#define STATIC_PATH_MAX 4096        /* 상대 경로 최대 길이 (URI 상한 2048 + index 이름을 넉넉히 담음) */
#define STATIC_HEADERS_MAX 2400     /* 추가 헤더 버퍼: "Location: <URI 2048>/\r\n" 이 들어갈 만큼 */

/* 문서 루트 디렉터리를 연다 (O_DIRECTORY). 성공 시 fd, 실패 -1 (errno). 프로그램 끝에 close */
int static_open_root(const char *dir);

/* URI → 문서 루트 기준 상대 경로 (앞에 '/' 없음, "." / ".." / 빈 세그먼트 없음).
 *   - origin-form("/a/b?x=1") 과 absolute-form("http://host/a/b") 둘 다 받는다. '?' 뒤 쿼리, '#' 뒤는 버린다.
 *   - %XX 디코딩 ("%20" → ' ', "%2e%2e" → ".." 도 디코딩 뒤에 검사하므로 우회 불가)
 *   - "/a/./b" → "a/b",  "/a/../b" → "b" (루트 안에서의 ".." 는 정리)
 *   - 디렉터리 요청 (끝이 '/' 이거나 "." / ".." 로 끝남) → 끝에 STATIC_INDEX 를 붙이고 *dir_request = 1
 * dir_request 는 NULL 이어도 됨.
 * 반환: 0 = 성공, 그 외 = 보낼 HTTP 상태 코드
 *   400 '/' 로 시작하지 않음 / 잘못된 %XX / %00(NUL)
 *   403 루트 밖으로 나가는 ".." / 숨김 파일·디렉터리(".git", ".env" 등. 단 ".well-known" 은 허용)
 *   414 결과가 cap 에 안 들어감 */
int static_resolve(const char *uri, char *out, size_t cap, int *dir_request);

/* static_lookup 의 결과 */
typedef struct {
    int status;               /* 200 / 301 / 304 / 400 / 403 / 404 / 414 / 500 */
    int fd;                   /* 200 이면 읽기 전용으로 연 파일 (호출자가 보낸 뒤 close). 그 외 -1 */
    size_t size;              /* 200 이면 파일 크기 (= Content-Length) */
    const char *content_type; /* 200 이면 MIME 타입 (정적 문자열), 그 외 NULL */
    char headers[STATIC_HEADERS_MAX]; /* 추가 헤더 ("이름: 값\r\n" 연속): 200/304 는 Last-Modified, 301 은 Location. 없으면 "" */
} static_result;

/* 문서 루트 root_fd 아래에서 uri 에 해당하는 파일을 찾는다.
 * if_modified_since: 요청의 If-Modified-Since 헤더 값 (없으면 NULL). 파일이 그 뒤로 안 바뀌었으면 304.
 * 결과 상태:
 *   200 일반 파일 → out->fd 로 본문을 보내면 됨
 *   301 디렉터리인데 끝에 '/' 가 없음 → Location: <uri>/ (상대 링크가 올바로 풀리도록)
 *   304 캐시 그대로 사용 (본문 없음)
 *   403 권한 없음(EACCES) / 숨김 파일 / 루트 탈출 / 일반 파일이 아님(FIFO, 장치 등)
 *   404 없음 (디렉터리에 index.html 이 없는 경우 포함 — 목록은 보여 주지 않음)
 *   500 그 밖의 오류 (fd 고갈 등) */
void static_lookup(int root_fd, const char *uri, const char *if_modified_since, static_result *out);

#endif /* STATIC_FILE_H */
