| 확인 | 직접(B:18831) | 프록시 경유(F:18830) |
|---|---|---|
| 로그인 상태 코드 | HTTP/1.1 303 See Other | HTTP/1.1 303 See Other |
| Set-Cookie 속성 | Path=/; HttpOnly; SameSite=Lax | Path=/; HttpOnly; SameSite=Lax |
| sid 길이 | 64 | 64 |
| Cache-Control | no-store | no-store |
| 응답 Connection | keep-alive | close |
| /me (Cookie 없음) | 401 | 401 |
| /me (자기 경로에서 받은 sid) | 200 | 200 |
| /me (다른 경로에서 받은 sid) | 200 | 200 |
| /me 본문 | user: alice | user: alice |
| /me Cache-Control | no-store | no-store |
| 로그아웃 Set-Cookie | Max-Age=0 | Max-Age=0 |
| 로그아웃 후 /me (옛 sid) | 401 | 401 |
| 정적 파일 / (회귀) | 200 | 200 |

프록시 로그에 Session ID 평문: 0건, 백엔드 로그: 0건
