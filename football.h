/*
 * football.h — 축구 경기 데모의 라우터 (경로 → 응답 결정)
 *
 * 지원하는 경로:
 *   GET        /, /matches/1           → 경기 화면 HTML (web/match.html)
 *   GET        /api/matches/1          → 경기 정보 JSON
 *   GET, POST  /api/matches/1/messages → 채팅 목록 조회 / 메시지 저장 (메모리)
 *   그 외                               → 404
 */
#ifndef MICROHTTP_FOOTBALL_H
#define MICROHTTP_FOOTBALL_H

#include "http.h"

/*
 * 요청을 보고 response를 채웁니다. 현재 단일 스레드 서버를 가정합니다.
 * 응답 본문이 malloc으로 만들어질 수 있으므로(채팅 JSON),
 * 응답을 다 보낸 뒤에는 반드시 football_response_release()를 호출해 정리하세요.
 */
void football_route(const HttpRequest *request, HttpResponse *response);
void football_response_release(HttpResponse *response);

#endif
