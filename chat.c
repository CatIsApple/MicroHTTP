/*
 * chat.c — 채팅 메시지 저장소 구현
 *
 * 하는 일은 세 가지입니다.
 *   1. 들어온 텍스트가 올바른 UTF-8인지 검사하기 (valid_text)
 *   2. 링 버퍼에 메시지 추가하기 (chat_append)
 *   3. 메시지를 JSON으로 바꾸기 (chat_message_json, chat_list_json)
 */
#include "chat.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * text가 "보여줄 만한 올바른 UTF-8 텍스트"인지 검사합니다. 맞으면 1, 아니면 0.
 *
 * UTF-8은 글자 하나를 1~4바이트로 표현합니다. 첫 바이트를 보면 몇 바이트짜리인지 알 수 있습니다.
 *   0xxxxxxx                              → 1바이트 (ASCII)
 *   110xxxxx 10xxxxxx                     → 2바이트 (첫 바이트 0xC2~0xDF)
 *   1110xxxx 10xxxxxx 10xxxxxx            → 3바이트 (0xE0~0xEF, 한글이 여기)
 *   11110xxx 10xxxxxx 10xxxxxx 10xxxxxx   → 4바이트 (0xF0~0xF4, 이모지가 여기)
 * 뒤따르는 바이트(continuation)는 항상 0x80~0xBF 범위입니다.
 *
 * RFC 3629 기준으로 다음과 같은 "가짜/위험한" UTF-8을 거절합니다.
 *   - 앞 바이트 없이 홀로 나온 continuation 바이트 (예: "\x80")
 *   - overlong: 더 짧게 쓸 수 있는 글자를 일부러 길게 쓴 것 (예: "\xC0\xAF" = '/')
 *   - surrogate: UTF-16 전용 영역 U+D800~U+DFFF (예: "\xED\xA0\x80")
 *   - U+10FFFF보다 큰 값 (예: "\xF4\x90\x80\x80")
 * 또한 탭/줄바꿈을 제외한 제어문자는 거절하고,
 * 공백·탭·줄바꿈만 있는 "보이지 않는" 메시지도 거절합니다.
 */
static int valid_text(const unsigned char *text, size_t length)
{
    int visible = 0; /* 눈에 보이는 글자가 하나라도 있었는지 */
    for (size_t i = 0; i < length;) {
        unsigned char c = text[i++];
        if (c < 0x80) {
            /* ASCII: 탭·CR·LF 외의 제어문자(0x00~0x1F)와 DEL(0x7F)은 거절 */
            if ((c < 0x20 && c != '\t' && c != '\r' && c != '\n') || c == 0x7f) return 0;
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') visible = 1;
            continue;
        }
        /*
         * 여러 바이트 글자: count = 뒤따를 바이트 수.
         * low~high는 "두 번째 바이트"에 허용되는 범위로, 특정 첫 바이트에서만 좁아집니다.
         */
        unsigned count;
        unsigned char low = 0x80, high = 0xbf;
        if (c >= 0xc2 && c <= 0xdf) count = 1;      /* 0xC0, 0xC1은 항상 overlong이라 제외 */
        else if (c >= 0xe0 && c <= 0xef) {
            count = 2;
            if (c == 0xe0) low = 0xa0;   /* overlong 방지 */
            if (c == 0xed) high = 0x9f;  /* surrogate(U+D800~) 방지 */
        } else if (c >= 0xf0 && c <= 0xf4) {
            count = 3;
            if (c == 0xf0) low = 0x90;   /* overlong 방지 */
            if (c == 0xf4) high = 0x8f;  /* U+10FFFF 초과 방지 */
        } else return 0;                 /* 홀로 나온 continuation(0x80~0xBF) 또는 0xF5 이상 */
        /* 남은 바이트가 부족하거나 두 번째 바이트가 범위 밖이면 거절 (예: 잘린 이모지 "\xF0\x9F") */
        if (length - i < count || text[i] < low || text[i] > high) return 0;
        /* 세 번째, 네 번째 바이트는 일반 continuation 범위(0x80~0xBF)인지만 확인 */
        for (unsigned j = 1; j < count; ++j)
            if (text[i + j] < 0x80 || text[i + j] > 0xbf) return 0;
        i += count;
        visible = 1; /* ASCII가 아닌 글자는 보이는 글자로 취급 */
    }
    return visible;
}

int chat_append(ChatStore *store, const unsigned char *text, size_t length,
                uint64_t created_at_ms, ChatMessage *created)
{
    /* 먼저 모든 검사를 끝냅니다. 실패하면 store를 건드리지 않고 바로 돌아갑니다. */
    if (length > CHAT_MAX_TEXT) return 413;
    if (!length || !valid_text(text, length)) return 400;
    if (store->last_id == UINT64_MAX) return 503;

    /*
     * 새 메시지가 들어갈 칸 = 가장 오래된 칸(start)에서 count만큼 뒤.
     * % CHAT_CAPACITY 덕분에 배열 끝을 넘으면 0번 칸으로 돌아갑니다(원형).
     * 가득 찬 상태라면 이 칸이 바로 start 칸, 즉 가장 오래된 메시지 자리입니다.
     */
    size_t slot = (store->start + store->count) % CHAT_CAPACITY;
    ChatMessage *message = &store->messages[slot];
    message->id = ++store->last_id;
    message->created_at_ms = created_at_ms;
    message->length = length;
    memcpy(message->text, text, length); /* 원본을 복사해 둡니다 */
    message->text[length] = 0;

    if (store->count == CHAT_CAPACITY)
        /* 가득 찼다면 가장 오래된 것을 덮어썼으므로 start를 한 칸 앞으로 옮깁니다. */
        store->start = (store->start + 1) % CHAT_CAPACITY;
    else ++store->count;
    *created = *message; /* 호출자에게 저장된 내용의 복사본을 돌려줍니다 */
    return 201;
}

/*
 * JSON 문자열을 조금씩 이어 붙이기 위한 작은 버퍼입니다.
 *   data     : 미리 할당한 메모리
 *   capacity : 전체 크기
 *   used     : 지금까지 쓴 바이트 수
 *   failed   : 한 번이라도 공간이 모자랐으면 1 (이후 append는 아무 일도 하지 않음)
 */
typedef struct { unsigned char *data; size_t capacity; size_t used; int failed; } Json;

/* printf처럼 형식 문자열을 받아 json 버퍼 끝에 이어 붙입니다. */
static void append(Json *json, const char *format, ...)
{
    if (json->failed) return;
    va_list args;
    va_start(args, format);
    int n = vsnprintf((char *)json->data + json->used, json->capacity - json->used, format, args);
    va_end(args);
    /* 공간이 모자라 잘렸으면 실패로 표시합니다. 반쪽짜리 JSON을 내보내지 않기 위해서입니다. */
    if (n < 0 || (size_t)n >= json->capacity - json->used) json->failed = 1;
    else json->used += (size_t)n;
}

/*
 * 메시지 하나를 {"id":1,"created_at_ms":...,"text":"..."} 형태로 씁니다.
 * text 안의 특수문자는 JSON 규칙에 맞게 바꿉니다(escape).
 *   "  → \"      \  → \\      제어문자(탭, 줄바꿈 등) → \u0009 같은 형태
 * UTF-8 한글/이모지는 이미 검증된 바이트라 그대로 씁니다.
 */
static void write_message(Json *json, const ChatMessage *message)
{
    append(json, "{\"id\":%" PRIu64 ",\"created_at_ms\":%" PRIu64 ",\"text\":\"",
           message->id, message->created_at_ms);
    for (size_t i = 0; i < message->length; ++i) {
        unsigned char c = message->text[i];
        if (c == '"' || c == '\\') append(json, "\\%c", c);
        else if (c < 0x20) append(json, "\\u%04x", (unsigned)c);
        else append(json, "%c", c);
    }
    append(json, "\"}");
}

/* 버퍼 작성을 마무리합니다. 실패했다면 메모리를 정리하고 NULL을 돌려줍니다. */
static unsigned char *finish(Json *json, size_t *length)
{
    if (json->failed) { free(json->data); *length = 0; return NULL; }
    *length = json->used;
    return json->data;
}

unsigned char *chat_message_json(const ChatMessage *message, size_t *length)
{
    *length = 0;
    /*
     * 필요한 최대 크기를 미리 계산해 한 번에 할당합니다.
     * 최악의 경우 한 바이트가 "\u0009"처럼 6바이트가 되므로 본문 × 6,
     * 여기에 id·시각·필드 이름 등을 위한 여유 160바이트를 더합니다.
     */
    Json json = {.capacity = CHAT_MAX_TEXT * 6 + 160};
    json.data = malloc(json.capacity);
    if (!json.data) return NULL;
    write_message(&json, message);
    return finish(&json, length);
}

unsigned char *chat_list_json(const ChatStore *store, size_t *length)
{
    *length = 0;
    /* 메시지 최대 개수 × 메시지 하나의 최대 JSON 크기 + 바깥 필드용 여유 256바이트 */
    Json json = {.capacity = CHAT_CAPACITY * (CHAT_MAX_TEXT * 6 + 160) + 256};
    json.data = malloc(json.capacity);
    if (!json.data) return NULL;
    /* 결과 예: {"match_id":1,"capacity":100,"max_text_bytes":512,"messages":[{...},{...}]} */
    append(&json, "{\"match_id\":1,\"capacity\":%d,\"max_text_bytes\":%d,\"messages\":[",
           CHAT_CAPACITY, CHAT_MAX_TEXT);
    /* 가장 오래된 메시지(start)부터 순서대로, 원형으로 돌며 count개를 씁니다. */
    for (size_t i = 0; i < store->count; ++i) {
        if (i) append(&json, ",");
        write_message(&json, &store->messages[(store->start + i) % CHAT_CAPACITY]);
    }
    append(&json, "]}");
    return finish(&json, length);
}
