/*
 * chat.h — 경기 채팅 메시지 저장소 (메모리 전용)
 *
 * 최근 메시지 CHAT_CAPACITY개만 보관하는 "링 버퍼(원형 버퍼)"입니다.
 * 가득 차면 가장 오래된 메시지를 덮어씁니다. 디스크에 저장하지 않으므로
 * 서버를 다시 시작하면 메시지는 모두 사라집니다.
 */
#ifndef MICROHTTP_CHAT_H
#define MICROHTTP_CHAT_H

#include <stddef.h>
#include <stdint.h>

enum {
    CHAT_CAPACITY = 100, /* 보관할 최대 메시지 수 */
    CHAT_MAX_TEXT = 512  /* 메시지 하나의 최대 길이 (UTF-8 바이트 기준, 글자 수 아님) */
};

/* 메시지 한 개 */
typedef struct {
    uint64_t id;                         /* 1부터 하나씩 증가하는 고유 번호 */
    uint64_t created_at_ms;              /* 저장된 시각 (1970-01-01 이후 밀리초) */
    size_t length;                       /* text의 바이트 수 ('\0' 제외) */
    unsigned char text[CHAT_MAX_TEXT + 1]; /* 본문 + 끝 '\0' */
} ChatMessage;

/*
 * 메시지 보관함 (링 버퍼)
 *
 *   messages: [ m101 | m102 | m3 | m4 | ... | m100 ]
 *                            ↑ start (가장 오래된 메시지 위치)
 *
 * start에서 시작해 count개를 차례로 읽으면(끝에 닿으면 0으로 돌아감) 오래된 순서가 됩니다.
 */
typedef struct {
    ChatMessage messages[CHAT_CAPACITY];
    size_t start;     /* 가장 오래된 메시지의 칸 번호 */
    size_t count;     /* 현재 보관 중인 메시지 수 (0 ~ CHAT_CAPACITY) */
    uint64_t last_id; /* 마지막으로 발급한 id (0이면 아직 없음) */
} ChatStore;

/*
 * 사용법: ChatStore store = {0}; 처럼 0으로 초기화해서 씁니다.
 * 주의: 단일 스레드용입니다. 여러 스레드에서 동시에 쓰려면 잠금(mutex) 같은 보호가 필요합니다.
 *
 * chat_append — 메시지 하나를 추가합니다.
 * 반환값은 그대로 HTTP 상태 코드로 쓸 수 있습니다.
 *   201: 성공 (*created에 저장된 메시지의 복사본이 들어감)
 *   400: 비어 있거나 공백뿐이거나 올바르지 않은 UTF-8
 *   413: CHAT_MAX_TEXT 바이트 초과
 *   503: id를 더 발급할 수 없음 (사실상 일어나지 않지만 안전하게 처리)
 * 실패하면 저장소를 전혀 바꾸지 않습니다.
 * 성공하면 text를 복사해 두므로, 호출 뒤 원본 text를 바꾸거나 해제해도 괜찮습니다.
 */
int chat_append(ChatStore *store, const unsigned char *text, size_t length,
                uint64_t created_at_ms, ChatMessage *created);

/*
 * 메시지(또는 전체 목록)를 JSON 문자열로 만들어 새 메모리에 담아 돌려줍니다.
 * - 돌려받은 메모리는 호출자가 free해야 합니다.
 * - 메모리 할당이나 변환에 실패하면 NULL.
 * - 성공하면 *length에 JSON 길이(끝 '\0' 제외)가 들어갑니다.
 */
unsigned char *chat_message_json(const ChatMessage *message, size_t *length);
unsigned char *chat_list_json(const ChatStore *store, size_t *length);

#endif
