/*
 * football.c — 축구 경기 데모 라우터 구현
 *
 * HTTP 파서가 해석한 요청(HttpRequest)을 받아서
 * "어떤 상태 코드로, 어떤 본문을 돌려줄지"(HttpResponse)를 정합니다.
 * 실제로 소켓에 쓰는 일은 server.c가 합니다.
 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime, strncasecmp 등 POSIX 함수를 쓰기 위한 선언 */
#include "football.h"
#include "chat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/*
 * 빌드할 때 tools/embed_page.py가 web/match.html을 C 바이트 배열로 바꿔 만든 파일입니다.
 * 이 파일 안에 `static const unsigned char match_page[]`가 정의되어 있어,
 * 서버 실행 파일 안에 HTML이 통째로 들어갑니다(실행 중 파일을 읽을 필요가 없음).
 */
#include "football_page.inc"

/*
 * 채팅 메시지 저장소. 프로그램이 실행되는 동안만 유지됩니다.
 * 지금은 요청을 하나씩 순서대로 처리하는 서버라 안전하지만,
 * 나중에 동시 처리(스레드 등)를 추가하면 잠금 같은 보호가 필요합니다.
 */
static ChatStore chat_store;
/* GET /api/matches/1 에 돌려줄 고정 경기 정보 (가상 데이터) */
static const unsigned char match_json[] =
    "{\"id\":1,\"home_team\":\"Home FC\",\"away_team\":\"Away FC\","
    "\"status\":\"scheduled\",\"is_demo\":true}\n";
static const unsigned char not_found[] = "404 Not Found\n";
static const unsigned char method_not_allowed[] = "405 Method Not Allowed\n";

/*
 * 요청 경로(target)가 path와 같은지 비교합니다. 쿼리 문자열('?' 뒤)은 무시합니다.
 * 예: target "/matches/1?view=main", path "/matches/1" → 같음
 */
static int path_is(const char *target, const char *path)
{
    size_t length = strcspn(target, "?"); /* '?' 앞까지의 길이 */
    return length == strlen(path) && memcmp(target, path, length) == 0;
}

/* 공백과 탭을 건너뛴 위치를 돌려줍니다. (OWS = Optional WhiteSpace) */
static const char *skip_ows(const char *p)
{
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

/*
 * Content-Type이 UTF-8 일반 텍스트인지 확인합니다. 대소문자와 공백은 너그럽게 봅니다.
 * 허용 예: "text/plain"
 *          "text/plain; charset=utf-8"
 *          "TEXT/PLAIN ; Charset = \"UTF-8\""
 * 거절 예: "application/json", "text/plain; charset=euc-kr"
 */
static int plain_utf8(const char *content_type)
{
    const char *p = content_type;
    if (strncasecmp(p, "text/plain", 10)) return 0;
    p = skip_ows(p + 10);
    if (!*p) return 1;                    /* "text/plain"만 있으면 OK */
    if (*p++ != ';') return 0;            /* 뒤에 무언가 있다면 ';'로 시작해야 함 */
    p = skip_ows(p);
    if (strncasecmp(p, "charset", 7)) return 0;
    p = skip_ows(p + 7);
    if (*p++ != '=') return 0;
    p = skip_ows(p);
    int quoted = *p == '"';               /* charset 값은 따옴표로 감쌀 수도 있음 */
    if (quoted) ++p;
    if (strncasecmp(p, "utf-8", 5)) return 0;
    p += 5;
    if (quoted && *p++ != '"') return 0;  /* 여는 따옴표가 있었다면 닫는 따옴표도 있어야 함 */
    return *skip_ows(p) == 0;             /* 남은 것은 공백뿐이어야 함 */
}

/* 정적 문자열로 된 JSON 오류 응답을 채웁니다. (메모리 할당 없음) */
static void json_error(HttpResponse *response, int status, const char *body)
{
    response->status = status;
    response->content_type = "application/json; charset=utf-8";
    response->body = (const unsigned char *)body;
    response->body_length = strlen(body);
}

/*
 * /api/matches/1/messages 처리
 *   GET  → 저장된 메시지 목록을 JSON으로 (200)
 *   POST → 본문 텍스트를 새 메시지로 저장하고, 저장된 메시지를 JSON으로 (201)
 *   그 외 → 405
 */
static void route_messages(const HttpRequest *request, HttpResponse *response)
{
    size_t length = 0;
    unsigned char *json;
    if (!strcmp(request->method, "GET")) {
        json = chat_list_json(&chat_store, &length);
        response->status = 200;
    } else if (!strcmp(request->method, "POST")) {
        /* 1) 본문 형식 확인: UTF-8 텍스트만 받습니다. */
        if (!plain_utf8(request->content_type)) {
            json_error(response, 415, "{\"error\":\"Content-Type은 text/plain; charset=utf-8을 사용하세요.\"}");
            return;
        }
        /* 2) 현재 시각을 밀리초 단위로 구합니다. (실제 시계 기준, 메시지 작성 시각 표시용) */
        struct timespec now;
        if (clock_gettime(CLOCK_REALTIME, &now) == -1 || now.tv_sec < 0) {
            json_error(response, 500, "{\"error\":\"서버 시간을 확인할 수 없습니다.\"}");
            return;
        }
        uint64_t time_ms = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
        /* 3) 저장소에 추가합니다. chat_append의 반환값이 곧 HTTP 상태 코드입니다. */
        ChatMessage created;
        int status = chat_append(&chat_store, request->body, request->body_length, time_ms, &created);
        if (status != 201) {
            /* 상태 코드에 맞는 한국어 오류 메시지를 고릅니다. */
            json_error(response, status, status == 413 ?
                "{\"error\":\"메시지는 UTF-8 512바이트 이하여야 합니다.\"}" : status == 400 ?
                "{\"error\":\"비어 있지 않은 올바른 UTF-8 텍스트를 입력하세요.\"}" :
                "{\"error\":\"지금은 메시지를 저장할 수 없습니다.\"}");
            return;
        }
        /* 서버 로그에는 메시지 내용 대신 id와 크기만 남깁니다. */
        fprintf(stderr, "[chat] stored_id=%llu bytes=%zu retained=%zu\n",
                (unsigned long long)created.id, created.length, chat_store.count);
        json = chat_message_json(&created, &length);
        response->status = 201;
    } else {
        response->status = 405;
        response->body = method_not_allowed;
        response->body_length = sizeof(method_not_allowed) - 1;
        response->allow = "GET, POST"; /* 405 응답에는 허용 메서드를 알려줘야 합니다 */
        return;
    }
    /* JSON 생성(메모리 할당)에 실패한 경우 */
    if (!json) {
        json_error(response, 500, "{\"error\":\"응답 메모리를 확보할 수 없습니다.\"}");
        return;
    }
    /*
     * json은 malloc으로 만든 메모리이므로 owned_body에도 기록해 둡니다.
     * 송신이 끝나면 football_response_release()가 이것을 free합니다.
     */
    response->body = response->owned_body = json;
    response->body_length = length;
    response->content_type = "application/json; charset=utf-8";
}

void football_route(const HttpRequest *request, HttpResponse *response)
{
    /* 기본값은 404. 아래에서 맞는 경로를 찾으면 덮어씁니다. */
    *response = (HttpResponse){
        .status = 404, .content_type = "text/plain; charset=utf-8",
        .body = not_found, .body_length = sizeof(not_found) - 1
    };
    /* 채팅 API는 GET과 POST를 모두 받으므로 따로 처리합니다. */
    if (path_is(request->target, "/api/matches/1/messages")) {
        route_messages(request, response);
        return;
    }
    int page = path_is(request->target, "/matches/1") || path_is(request->target, "/"); /* HTML 화면 */
    int info = path_is(request->target, "/api/matches/1");                              /* 경기 정보 JSON */
    if (!page && !info) return; /* 모르는 경로 → 404 그대로 */
    /* 화면과 경기 정보는 읽기 전용이라 GET만 허용합니다. */
    if (strcmp(request->method, "GET")) {
        response->status = 405;
        response->body = method_not_allowed;
        response->body_length = sizeof(method_not_allowed) - 1;
        response->allow = "GET";
        return;
    }
    response->status = 200;
    /* 두 배열 모두 끝에 '\0'이 있으므로 sizeof - 1이 실제 내용 길이입니다. */
    response->body = page ? match_page : match_json;
    response->body_length = page ? sizeof(match_page) - 1 : sizeof(match_json) - 1;
    response->content_type = page ? "text/html; charset=utf-8" : "application/json; charset=utf-8";
}

/* 응답에 딸린 동적 메모리를 해제합니다. owned_body가 NULL이면 free(NULL)이라 안전합니다. */
void football_response_release(HttpResponse *response)
{
    free(response->owned_body);
    response->owned_body = NULL;
    response->body = NULL;
    response->body_length = 0;
}
