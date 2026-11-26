# proxy_core 함수 정리

`src/proxy_core.c`, `include/proxy_core.h`.
리버스(rproxy)와 포워드(fproxy)가 같이 쓰는 부품. 상대 서버와 요청 하나를 주고받음.
- 목적지 고르기, connect: 각 모듈이 함.
- 그 뒤 보내기, 받기, 중계: core가 함.

```
rproxy_handle: 백엔드 고르고 connect
        ↓ bfd
pxy_exchange ─ pxy_build_request ──> 상대에게 전송
             ─ pxy_parse_response_head <── 응답 머리 수신
             ─ pxy_rewrite_response_head ──> 클라이언트에게 전송
             ─ 본문 중계 → bfd 닫음
```

## 공개 함수

### `pxy_build_request(out, cap, req, target, client_ip, default_host)`
상대 서버로 보낼 요청 머리를 만듦.

| 인자                         | 뜻                                                                  |
| -------------------------- | ------------------------------------------------------------------ |
| `char *out`                | 결과 쓸 버퍼                                                            |
| `size_t cap`               | `out` 크기                                                           |
| `const http_request *req`  | 파싱된 클라이언트 요청                                                       |
| `const char *target`       | 요청 줄 경로. 리버스는 `req->uri` 그대로 줌. 포워드는 `/a` 같은 origin-form으로 바꿔서 줄 것 |
| `const char *client_ip`    | `X-Forwarded-For`에 덧붙일 IP                                          |
| `const char *default_host` | 요청에 Host가 없을 때 대신 쓸 값                                              |

- 빼는 헤더:
  - hop-by-hop 헤더(`Connection`, `Keep-Alive`, `Proxy-Connection`, `TE`, `Trailer`, `Upgrade`)
  - `Transfer-Encoding`, `Expect`
  - `Connection`에 이름이 적힌 헤더
  - 클라이언트가 보낸 `X-Forwarded-For`, `X-Forwarded-Proto`
- 넣는 헤더:
  - `X-Forwarded-For`: 기존 값 뒤에 클라이언트 IP를 붙임
  - `X-Forwarded-Proto: http`
  - `Connection: close`
  - `Host`: 요청에 없을 때만
- 반환: 쓴 길이. 버퍼가 모자라면 -1.

### `pxy_parse_response_head(buf, head_len, info)`
응답 머리를 검사하고 해석함.

| 인자 | 뜻 |
|---|---|
| `const char *buf` | 응답 머리 원문 |
| `size_t head_len` | 머리 길이. 빈 줄까지 포함 |
| `pxy_resp_info *info` | 결과: `status`, `has_length`, `length`, `chunked`, `has_te` |

- 거절하는 경우:
  - 상태 줄이 이상하거나 상태 코드가 100~599 밖
  - 줄 안에 CR/LF/NUL이 섞임 (응답 분할 공격 방지)
  - 콜론 없는 줄, 줄 이어 쓰기(obs-fold)
  - Content-Length가 숫자가 아니거나, 여러 개인데 값이 서로 다름
- Transfer-Encoding이 있으면 Content-Length는 무시함.
- 반환: 0이면 정상. -1이면 형식이 틀린 것이고, 결국 502가 나감.

### `pxy_rewrite_response_head(out, cap, buf, head_len, keep_alive)`
클라이언트에게 보낼 응답 머리를 만듦.

| 인자 | 뜻 |
|---|---|
| `out`, `cap` | 결과 버퍼, 크기 |
| `buf`, `head_len` | 상대 응답 머리 원문, 길이 |
| `int keep_alive` | 1이면 `Connection: keep-alive`, 0이면 `Connection: close`를 붙임 |

- 상태 줄은 `HTTP/1.1 <원래 코드·문구>`로 만듦.
- hop-by-hop 헤더와 `Connection`에 이름이 적힌 헤더는 뺌.
- 반환: 쓴 길이. 실패하면 -1.

### `pxy_exchange(bfd, cfd, req, target, client_ip, default_host, backend_timeout_ms, client_timeout_ms, keep, status)`
요청을 보내고, 응답을 받고, 클라이언트로 중계함. 본체.

| 인자 | 뜻 |
|---|---|
| `int bfd` | 이미 connect된 상대 소켓. 끝나면 이 함수가 닫음 |
| `int cfd` | 클라이언트 소켓 |
| `req`, `target`, `client_ip`, `default_host` | `pxy_build_request`에 그대로 넘김 |
| `int backend_timeout_ms` | 상대 응답을 기다리는 최대 시간 |
| `int client_timeout_ms` | 클라이언트에게 보낼 때 최대 시간 |
| `int *keep` | 입력: keep-alive를 할지. 출력: 실제로 했는지. 길이를 모르는 응답이면 0으로 바뀜 |
| `int *status` | 출력: 클라이언트에 보낸 상태 코드 (로그용) |

순서:
1. 요청 머리와 본문을 보냄.
2. 응답 머리를 받음. 1xx는 건너뛰고, 101이 오면 502.
3. 본문 처리 방식을 정함.
   - 본문 없음: HEAD 요청, 204, 304
   - Content-Length만큼 중계: 클라이언트 keep-alive 유지
   - chunked이거나 길이가 없으면 상대가 닫을 때까지 중계: 클라이언트 연결도 닫음
4. 다시 쓴 머리와 이미 받은 본문 앞부분을 클라이언트에게 보냄.
5. 나머지 본문을 16KB 버퍼로 중계함.

반환:
- `0`: 다 보냄
- `>0` (502/504/500): 아직 아무것도 안 보냄. 호출한 쪽이 이 코드로 오류 응답을 보냄
- `-1`: 중간에 실패해 응답이 잘림. 연결을 끊음

## 내부 도우미 (static)

| 함수 | 인자 | 하는 일 |
|---|---|---|
| `lower(c)` | 문자 하나 | ASCII 소문자로 바꿈 |
| `ieq_n(a, an, b)` | `a`는 길이 `an`, `b`는 `\0`으로 끝남 | 대소문자 무시하고 같은지 |
| `ieq(a, b)` | 둘 다 `\0`으로 끝남 | 대소문자 무시하고 같은지 |
| `list_has(list, len, name, nlen)` | 쉼표 목록, 찾을 토큰 | `"close, X-Foo"` 안에 `name`이 있는지 |
| `is_hop_by_hop(name, nlen)` | 헤더 이름 | hop-by-hop 헤더 6개 중 하나인지 |
| `sb_put(b, s, n)` | `sbuf`, 데이터, 길이 | 버퍼에 덧붙임. 넘치면 `err`만 켜고 쓰지 않음 |
| `sb_str(b, s)` | `sbuf`, 문자열 | `strlen`으로 길이를 재서 `sb_put` 호출 |
| `req_conn_lists(req, name)` | 요청, 헤더 이름 | 요청의 `Connection`에 `name`이 적혀 있는지 |
| `line_end(buf, pos, head_len)` | 버퍼, 시작 위치, 끝 | 다음 CRLF의 CR 위치. 없으면 `head_len` |
| `split_header(s, e, &name, &nlen, &val, &vlen)` | 줄 범위 `[s, e)`, 출력 4개 | 이름과 값으로 나누고 값 앞뒤 공백 제거. 형식이 틀리면 -1 |
| `resp_conn_lists(buf, head_len, name, nlen)` | 응답 머리, 헤더 이름 | 응답의 `Connection`에 `name`이 적혀 있는지 |

`sbuf` 구조체 필드:
- `p`: 버퍼
- `cap`: 크기
- `len`: 지금까지 쓴 길이
- `err`: 넘쳤는지
