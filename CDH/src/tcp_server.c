/* tcp_server.c — 소켓을 직접 다루는 저수준 함수들.
 * 서버 준비(socket/bind/listen), 연결 받기(accept), 마감 시간이 있는 송수신,
 * RST 없이 닫기를 맡는다. HTTP 내용은 전혀 모르고 바이트만 다룬다. */

#define _POSIX_C_SOURCE 200809L /* POSIX 2008 함수(clock_gettime, strtol 등)를 헤더에서 보이게 함. include 보다 먼저 와야 함 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE /* macOS: _POSIX_C_SOURCE 를 켜면 MSG_DONTWAIT, sendfile 같은 BSD 확장이 숨겨짐 → 다시 보이게 */
#endif
#if defined(__linux__)
#define _GNU_SOURCE /* 리눅스: accept4 (논블로킹·CLOEXEC 소켓을 시스템콜 한 번에), MSG_MORE */
#endif

/* 파일 본문 송신 방식. -DMICROHTTP_NO_SENDFILE 로 끄면 이식용 pread + sendmsg 경로 (리눅스에서 그 경로 테스트용) */
#if defined(__linux__) && !defined(MICROHTTP_NO_SENDFILE)
#define USE_LINUX_SENDFILE 1  /* sendfile(sock, file, &off, len) */
#elif defined(__APPLE__) && !defined(MICROHTTP_NO_SENDFILE)
#define USE_DARWIN_SENDFILE 1 /* sendfile(file, sock, off, &len, hdtr, 0) — 머리 동봉 가능 */
#endif

#include "tcp_server.h" /* 이 파일이 구현할 함수들의 선언 (선언과 구현이 일치하는지 컴파일러가 검사) */

#include <arpa/inet.h>  /* htons/htonl/ntohs(바이트 순서 변환), inet_ntop(IP 숫자 → 문자열) */
#include <errno.h>      /* errno 변수와 EINTR, EAGAIN, ETIMEDOUT 같은 에러 코드 */
#include <fcntl.h>      /* fcntl, O_NONBLOCK, FD_CLOEXEC (연결 소켓을 논블로킹으로) */
#include <netinet/in.h> /* struct sockaddr_in(IPv4 주소 구조체), INADDR_ANY, INET_ADDRSTRLEN */
#include <poll.h>       /* poll(), struct pollfd, POLLIN/POLLOUT (fd 가 준비될 때까지 기다리기) */
#include <stdio.h>      /* perror, snprintf */
#include <string.h>     /* memset, strstr */
#include <sys/socket.h> /* socket, bind, listen, accept, send, sendmsg, recv, shutdown, setsockopt, MSG_* 플래그 */
#include <sys/uio.h>    /* struct iovec (sendmsg 에 넘길 "여러 덩어리" 목록) */
#include <time.h>       /* clock_gettime, struct timespec, CLOCK_MONOTONIC */
#include <unistd.h>     /* close, pread */
#if defined(USE_LINUX_SENDFILE)
#include <sys/sendfile.h> /* sendfile (리눅스 전용 시그니처) */
#endif
#if defined(USE_DARWIN_SENDFILE)
#include <sys/types.h>    /* off_t, struct sf_hdtr 가 쓰는 타입 */
#endif

int tcp_listen(uint16_t port, int backlog) /* 리스닝 소켓을 만들어 fd 를 돌려줌, 실패 시 -1 */
{
    /* 1. socket(): IPv4 TCP 소켓 생성 */
    int fd = socket(AF_INET, SOCK_STREAM, 0); /* AF_INET = IPv4, SOCK_STREAM = TCP, 0 = 기본 프로토콜. 커널이 소켓 객체를 만들고 fd 번호를 줌 */
    if (fd < 0) {                             /* -1 이면 생성 실패(fd 고갈, 권한 등) */
        perror("socket");                     /* "socket: <에러 설명>" 을 stderr 에 출력 (errno 를 문장으로 바꿔 줌) */
        return -1;                            /* 호출자에게 실패를 알림 */
    }

    /* 서버 재시작 시 TIME_WAIT 상태 포트에 바로 bind 할 수 있도록 */
    int opt = 1;                                                           /* 옵션 값: 1 = 켜기 */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) { /* 소켓 수준(SOL_SOCKET) 옵션 SO_REUSEADDR 를 켬 */
        perror("setsockopt(SO_REUSEADDR)");                                /* 실패 이유 출력 */
        close(fd);                                                         /* 만든 소켓은 반납 (fd 누수 방지) */
        return -1;                                                         /* 실패 */
    }

    /* 2. bind(): 모든 인터페이스(0.0.0.0)의 port 에 바인딩 */
    struct sockaddr_in addr;                  /* IPv4 주소 정보 (주소 체계, IP, 포트) */
    memset(&addr, 0, sizeof(addr));           /* 구조체 전체를 0 으로 초기화 (안 쓰는 필드에 쓰레기 값 방지) */
    addr.sin_family = AF_INET;                /* 주소 체계: IPv4 */
    addr.sin_addr.s_addr = htonl(INADDR_ANY); /* IP = 0.0.0.0 (이 PC 의 모든 네트워크 카드에서 받음). htonl: 호스트 → 네트워크 바이트 순서(빅엔디언) */
    addr.sin_port = htons(port);              /* 포트 번호도 네트워크 바이트 순서로 변환 (htons = 16비트용) */

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { /* 소켓에 "이 IP:포트 는 내 것" 이라고 커널에 등록. 범용 sockaddr* 로 캐스팅 필요 */
        perror("bind");                                         /* 흔한 실패: "Address already in use" (EADDRINUSE) */
        close(fd);                                              /* 소켓 반납 */
        return -1;                                              /* 실패 */
    }

    /* 3. listen(): 커널이 3-way handshake 를 마친 연결을 backlog 큐에 쌓는다 */
    if (listen(fd, backlog) < 0) { /* 소켓을 LISTEN 상태로 바꿈. 이때부터 커널이 SYN 에 자동 응답 */
        perror("listen");          /* 실패 이유 출력 */
        close(fd);                 /* 소켓 반납 */
        return -1;                 /* 실패 */
    }

    return fd; /* 성공: 연결 요청만 받는 리스닝 소켓의 fd */
}

int tcp_accept(int listen_fd, char *client_addr_buf, size_t buf_len) /* 연결 하나를 받아 새 fd 를 돌려줌 */
{
    struct sockaddr_in cli;          /* 접속한 클라이언트의 주소가 여기 채워짐 */
    socklen_t cli_len = sizeof(cli); /* 입력: cli 크기 / 출력: 실제로 채워진 크기 (accept 가 갱신) */

    /* 4. accept(): 완료 큐에서 연결 하나를 꺼낸다 (없으면 블로킹)
     * 새 소켓은 논블로킹 + CLOEXEC 로 만든다.
     *   논블로킹: 모든 송수신이 어차피 "시도 → EAGAIN 이면 poll" 이라 블로킹일 이유가 없고,
     *            sendfile 때마다 fcntl 로 껐다 켰다 하는 시스템콜 2번을 없앤다.
     *   CLOEXEC: 나중에 CGI 등으로 fork + exec 할 때 자식에게 연결 소켓이 새지 않게. */
#if defined(__linux__)
    int cfd = accept4(listen_fd, (struct sockaddr *)&cli, &cli_len, SOCK_NONBLOCK | SOCK_CLOEXEC); /* 플래그까지 한 번에 */
    if (cfd < 0)                                                    /* 실패: 시그널(EINTR), fd 고갈(EMFILE) 등. errno 는 그대로 둠 */
        return -1;                                                  /* 호출자가 errno 를 보고 처리 */
#else
    int cfd = accept(listen_fd, (struct sockaddr *)&cli, &cli_len); /* handshake 가 끝난 연결을 꺼내 새 fd(cfd)로 줌. 큐가 비면 잠듦 */
    if (cfd < 0)                                                    /* 실패: 시그널(EINTR), fd 고갈(EMFILE) 등. errno 는 그대로 둠 */
        return -1;                                                  /* 호출자가 errno 를 보고 처리 */
    fcntl(cfd, F_SETFL, O_NONBLOCK);                                /* 새 소켓의 상태 플래그는 0 → O_NONBLOCK 만 켜면 됨 */
    fcntl(cfd, F_SETFD, FD_CLOEXEC);                                /* exec 시 자동으로 닫힘 */
#endif

    if (client_addr_buf && buf_len > 0) {                                                   /* 호출자가 주소 문자열을 원할 때만 */
        char ip[INET_ADDRSTRLEN] = "?";                                                     /* IPv4 문자열 최대 길이(16) 버퍼. 변환 실패 대비 기본값 "?" */
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));                                  /* 4바이트 숫자 IP → "127.0.0.1" 같은 문자열 */
        snprintf(client_addr_buf, buf_len, "%s:%u", ip, (unsigned)ntohs(cli.sin_port));    /* "IP:포트" 로 합침. ntohs: 네트워크 → 호스트 바이트 순서. snprintf 는 buf_len 을 넘지 않음 */
    }
    return cfd; /* 이 클라이언트 전용 연결 소켓. 데이터는 이걸로 주고받음 */
}

/* ---- 전체 마감 시간(deadline) 도우미 ----
 * SO_RCVTIMEO 는 "recv 한 번" 의 대기 시간만 제한한다. 1바이트씩 천천히
 * 보내는 클라이언트(slowloris)는 매번 시간 안에 도착하므로 끝없이 버틴다.
 * 그래서 시작 시각 기준으로 남은 시간을 계산해 poll() 에 넘긴다. */
static long long now_ms(void) /* 현재 시각(ms). static = 이 파일 안에서만 쓰는 함수 */
{
    struct timespec ts;                                           /* 초(tv_sec) + 나노초(tv_nsec) */
    clock_gettime(CLOCK_MONOTONIC, &ts);                          /* MONOTONIC: 부팅 후 계속 증가하는 시계. 사용자가 시스템 시간을 바꿔도 거꾸로 가지 않음 */
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;    /* 초 → ms, 나노초 → ms 로 바꿔 더함. long long 으로 곱해 오버플로 방지 */
}

/* deadline 까지 fd 에 events 가 생기길 기다린다.
 * 1 = 준비됨, 0 = 마감 초과(errno = ETIMEDOUT), -1 = 오류 */
static int wait_until(int fd, short events, long long deadline) /* events: POLLIN(읽기 가능) 또는 POLLOUT(쓰기 가능) */
{
    for (;;) {                                          /* 시그널로 깨어나면 다시 기다리기 위한 무한 루프 */
        long long left = deadline - now_ms();           /* 마감까지 남은 시간(ms) */
        if (left <= 0) {                                /* 이미 마감 시간이 지났으면 */
            errno = ETIMEDOUT;                          /* 호출자가 "타임아웃" 임을 알 수 있게 에러 코드 설정 */
            return 0;                                   /* 0 = 시간 초과 */
        }
        struct pollfd pfd = {.fd = fd, .events = events}; /* 감시할 fd 와 기다릴 이벤트 */
        int r = poll(&pfd, 1, (int)left);               /* fd 1개를 최대 left ms 동안 기다림. 반환: 준비된 fd 수(1), 0 = 시간 초과, -1 = 오류 */
        if (r < 0 && errno == EINTR)                    /* 시그널(Ctrl+C 등) 때문에 중간에 깨어난 경우 */
            continue;                                   /* 남은 시간을 다시 계산해서 계속 기다림 */
        if (r == 0) {                                   /* poll 이 시간 초과로 끝남 */
            errno = ETIMEDOUT;                          /* 타임아웃 표시 */
            return 0;                                   /* 0 = 시간 초과 */
        }
        return r < 0 ? -1 : 1;                          /* r > 0 이면 준비됨(1), 그 외 오류면 -1. (상대가 끊은 경우도 "준비됨" 으로 오고 recv/send 에서 드러남) */
    }
}

/* ---- 송신: 모든 메모리 송신은 sendv_until 한 곳을 지난다 ---- */

/* MSG_DONTWAIT: 송신 버퍼가 일부만 비어 있어도 블로킹하지 않고 들어간 만큼만 복사 → 마감 시간을 넘겨 붙잡히지 않음.
 * MSG_NOSIGNAL(리눅스): 끊긴 소켓에 써도 SIGPIPE 대신 EPIPE. main 이 SIGPIPE 를 무시하지만 라이브러리만 써도 안전하게. */
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS (MSG_DONTWAIT | MSG_NOSIGNAL)
#else
#define SEND_FLAGS MSG_DONTWAIT
#endif

/* iov[0..cnt) 를 순서대로 deadline 까지 전부 보낸다. 성공 0, 실패·초과 -1.
 * extra_flags: sendmsg 에 더할 플래그 (리눅스 MSG_MORE 등, 없으면 0).
 * iov 는 진행 상황을 기록하느라 수정된다 (보낸 만큼 base 전진, len 감소).
 *
 * "먼저 보내 보고, 막히면(EAGAIN) 그때 poll" 순서다. 송신 버퍼는 거의 항상 비어 있으므로
 * 매번 poll 부터 하면 응답마다 쓸데없는 시스템콜이 하나씩 더 생긴다. */
static int sendv_until(int fd, struct iovec *iov, int cnt, int extra_flags, long long deadline)
{
    int idx = 0;                                /* 아직 다 안 보낸 첫 덩어리의 번호 */
    while (idx < cnt) {                         /* 덩어리가 남은 동안 */
        if (iov[idx].iov_len == 0) {            /* 길이 0 (처음부터 비었거나 다 보냄) */
            idx++;                              /* 다음 덩어리로 */
            continue;
        }
        struct msghdr msg;                      /* sendmsg 에 넘길 "여러 덩어리 + 옵션" 묶음 */
        memset(&msg, 0, sizeof(msg));           /* 안 쓰는 필드(주소 등)는 0 */
        msg.msg_iov = &iov[idx];                /* 남은 덩어리들의 시작 */
        msg.msg_iovlen = cnt - idx;             /* 남은 덩어리 수 */
        ssize_t n = sendmsg(fd, &msg, SEND_FLAGS | extra_flags); /* 남은 덩어리들을 한 번에 커널 송신 버퍼로 */
        if (n < 0) {                            /* 실패한 경우 */
            if (errno == EINTR)                 /* 시그널 → 바로 재시도 */
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {   /* 송신 버퍼 꽉 참 → 자리가 날 때까지 마감 안에서 기다림 */
                if (wait_until(fd, POLLOUT, deadline) <= 0)
                    return -1;                  /* 시간 초과(errno=ETIMEDOUT) 또는 오류 */
                continue;
            }
            return -1;                          /* EPIPE 등: 상대가 끊음 */
        }
        size_t left = (size_t)n;                /* 보낸 바이트 수를 덩어리 앞에서부터 차례로 차감 */
        while (idx < cnt && left > 0) {         /* 보낸 양이 남아 있는 동안 */
            if (left >= iov[idx].iov_len) {     /* 이 덩어리를 통째로 보냈으면 */
                left -= iov[idx].iov_len;       /* 그만큼 차감 */
                iov[idx].iov_len = 0;           /* 이 덩어리는 끝 */
                idx++;                          /* 다음 덩어리 */
            } else {                            /* 이 덩어리의 일부만 보냈으면 */
                iov[idx].iov_base = (char *)iov[idx].iov_base + left; /* 시작 위치를 보낸 만큼 앞으로 */
                iov[idx].iov_len -= left;       /* 남은 길이 감소 */
                left = 0;                       /* 차감할 양 소진 */
            }
        }
    }
    return 0; /* 전부 커널 송신 버퍼에 넣음 (상대가 받았다는 뜻은 아님) */
}

int tcp_sendv_all(int fd, struct iovec *iov, int iovcnt, int timeout_ms) /* 여러 덩어리를 이어서 전송 */
{
    return sendv_until(fd, iov, iovcnt, 0, now_ms() + timeout_ms); /* 전체 마감 = 지금 + timeout */
}

int tcp_send_all(int fd, const void *buf, size_t len, int timeout_ms) /* 덩어리 하나 = sendv 의 특수한 경우 */
{
    struct iovec iov = {.iov_base = (void *)buf, .iov_len = len}; /* iovec 은 const 가 아니라서 캐스팅 (읽기만 함) */
    return tcp_sendv_all(fd, &iov, 1, timeout_ms);                 /* 같은 송신 루프 사용 */
}

/* ---- 송신: 파일 본문 ----
 * 파일 송신의 마감은 "전체 고정" 이 아니다. 전체 고정이면 5초 안에 못 보내는 큰 파일(느린 회선의 100MB 등)은
 * 영원히 받을 수 없다. 대신 두 마감 중 이른 쪽에서 끊는다:
 *   (1) 무진척: 마지막으로 바이트가 나간 뒤 timeout_ms 동안 하나도 못 보냄
 *   (2) 최저 속도: 시작부터 지난 시간 > timeout_ms + 보낸 바이트 / TCP_SEND_MIN_RATE
 *       → 바이트를 찔끔찔끔 받아 (1) 을 계속 피하는 클라이언트도, 평균 속도가 하한 아래면 끊긴다.
 * (2) 가 없으면 한 연결씩 처리하는 이 서버를 느린 클라이언트 하나가 몇 시간이고 붙잡을 수 있다. */

/* 진척이 있을 때마다 시계를 읽지 않고, 막혔을 때만 읽는다 → 빠른 경로에서 clock_gettime 은 시작 1번뿐 */
typedef struct {
    long long start;         /* 송신 시작 시각 */
    long long idle_deadline; /* (1) 현재 무진척 마감 */
    unsigned long long sent; /* 지금까지 보낸 바이트 (머리 포함) */
    int progressed;          /* 마지막 대기 이후 바이트가 나갔는지 */
    int timeout_ms;
} send_clock;

static void clock_begin(send_clock *c, int timeout_ms)
{
    c->start = now_ms();
    c->idle_deadline = 0;
    c->sent = 0;
    c->progressed = 1;                           /* 첫 대기 때 무진척 마감을 잡게 */
    c->timeout_ms = timeout_ms;
}

static void clock_progress(send_clock *c, size_t n) /* n 바이트 나감 */
{
    c->sent += n;
    c->progressed = 1;
}

static long long clock_deadline(send_clock *c) /* 지금 기다릴 수 있는 마지막 시각 = (1), (2) 중 이른 쪽 */
{
    if (c->progressed) {                         /* 지난번 대기 뒤로 진척이 있었으면 */
        c->idle_deadline = now_ms() + c->timeout_ms; /* 무진척 마감을 지금부터 다시 */
        c->progressed = 0;
    }
    long long rate_deadline = c->start + c->timeout_ms + (long long)(c->sent * 1000 / TCP_SEND_MIN_RATE);
    return rate_deadline < c->idle_deadline ? rate_deadline : c->idle_deadline;
}

#if defined(USE_LINUX_SENDFILE) || defined(USE_DARWIN_SENDFILE)
static int clock_wait_writable(int fd, send_clock *c) /* 1 = 쓰기 가능, 0 = 초과(ETIMEDOUT), -1 = 오류 */
{
    return wait_until(fd, POLLOUT, clock_deadline(c));
}
#endif

#if !defined(USE_LINUX_SENDFILE) /* 리눅스 sendfile 빌드에선 안 씀 (macOS 는 fallback 으로 씀) */
/* 이식용 경로: pread 로 64KB 씩 읽어 sendmsg. 첫 조각은 머리(head)와 같은 sendmsg 로 보낸다
 * → 64KB 이하 파일은 머리 + 본문이 시스템콜 하나, 패킷 하나로 나간다.
 * len > 0 이어야 한다 (머리만 보내는 건 호출자가 처리). head 는 보낸 만큼 수정된다.
 * 마감은 조각 단위로 잰다 (조각 안에서 일부 나간 건 조각이 끝나야 반영 → 조각 하나만큼 조금 엄격). */
static int pread_send(int sock, struct iovec *head, int file_fd, off_t offset, size_t len, send_clock *c)
{
    char chunk[64 * 1024];                      /* 읽어 올 조각 버퍼 */
    while (len > 0) {                           /* 남은 바이트가 있는 동안 */
        size_t want = len < sizeof(chunk) ? len : sizeof(chunk); /* 이번에 읽을 양 */
        ssize_t r = pread(file_fd, chunk, want, offset);          /* offset 위치에서 읽기 (파일 위치는 안 바뀜) */
        if (r < 0) {                            /* 읽기 실패 */
            if (errno == EINTR)                 /* 시그널 */
                continue;                       /* 다시 */
            return -1;
        }
        if (r == 0) {                           /* 파일이 예상보다 짧음 (보내는 도중 잘림 등) */
            if (head->iov_len > 0)              /* 머리라도 보내 둠 (짧은 본문을 "성공" 으로 치지는 않음) */
                sendv_until(sock, head, 1, 0, now_ms() + c->timeout_ms);
            errno = EIO;                        /* Content-Length 를 지킬 수 없음 → 오류 */
            return -1;
        }
        struct iovec iov[2] = {*head, {.iov_base = chunk, .iov_len = (size_t)r}}; /* 남은 머리(처음만) + 이번 조각 */
        size_t piece = head->iov_len + (size_t)r;
        if (sendv_until(sock, iov, 2, 0, clock_deadline(c)) < 0) /* 무진척·최저 속도 마감 중 이른 쪽 */
            return -1;
        clock_progress(c, piece);
        head->iov_len = 0;                      /* 머리는 첫 조각과 함께 나감 */
        offset += r;                            /* 다음 읽을 위치 */
        len -= (size_t)r;                       /* 남은 길이 */
    }
    return 0;
}
#endif

#if defined(USE_LINUX_SENDFILE) || defined(USE_DARWIN_SENDFILE)
/* sendfile 에는 MSG_DONTWAIT 같은 플래그가 없어서, 마감 시간을 지키려면 소켓 자체가 논블로킹이어야 한다.
 * tcp_accept 가 준 소켓은 이미 논블로킹이라 F_GETFL 한 번으로 끝난다.
 * 블로킹 소켓(테스트의 socketpair 등)만 잠깐 바꿨다가 nonblock_end 에서 되돌린다. 성공 0, 실패 -1 */
static int nonblock_begin(int fd, int *saved_flags)
{
    int flags = fcntl(fd, F_GETFL, 0);          /* 현재 상태 플래그 */
    if (flags < 0)
        return -1;
    *saved_flags = flags;                       /* 복구용 */
    if (!(flags & O_NONBLOCK) && fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) /* 블로킹이면 논블로킹으로 */
        return -1;
    return 0;
}

static void nonblock_end(int fd, int saved_flags)
{
    if (!(saved_flags & O_NONBLOCK)) {          /* 원래 블로킹이었으면 */
        int saved_errno = errno;                /* fcntl 이 실패 이유(errno)를 덮지 않게 */
        fcntl(fd, F_SETFL, saved_flags);        /* 원래대로 */
        errno = saved_errno;
    }
}
#endif

#if defined(USE_LINUX_SENDFILE)
/* 리눅스: sendfile() 로 파일 → 소켓을 커널 안에서 바로 복사 (유저 공간 복사 0번, 페이지 캐시 → 소켓) */
static int linux_sendfile(int sock, int file_fd, off_t offset, size_t len, send_clock *c)
{
    int saved;
    if (nonblock_begin(sock, &saved) < 0)
        return -1;
    int rc = 0;
    while (len > 0) {                           /* 남은 바이트가 있는 동안 */
        ssize_t n = sendfile(sock, file_fd, &offset, len); /* offset 은 커널이 보낸 만큼 전진시켜 줌 (파일 자체 위치는 안 바뀜) */
        if (n < 0) {                            /* 실패 */
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) { /* 송신 버퍼 꽉 참 */
                if (clock_wait_writable(sock, c) <= 0) {
                    rc = -1;                    /* 무진척·최저 속도 시간 초과 또는 오류 */
                    break;
                }
                continue;
            }
            rc = -1;                            /* EPIPE 등 */
            break;
        }
        if (n == 0) {                           /* 파일이 예상보다 짧음 (보내는 도중 잘림 등) */
            errno = EIO;                        /* Content-Length 를 지킬 수 없음 → 오류 */
            rc = -1;
            break;
        }
        len -= (size_t)n;                       /* 남은 길이 감소 */
        clock_progress(c, (size_t)n);           /* 진척 있음 → 다음 대기 때 무진척 마감을 새로 */
    }
    nonblock_end(sock, saved);
    return rc;
}
#endif

#if defined(USE_DARWIN_SENDFILE)
/* macOS: sendfile(file, sock, offset, &len, hdtr, 0). 머리(hdtr.headers)를 같은 호출로 보낼 수 있다
 * → 머리 + 파일이 시스템콜 하나. 논블로킹 소켓에서 EAGAIN 이어도 *len 에 "머리 포함 보낸 바이트 수" 가 온다.
 * 주의: 입력 *len == 0 은 "파일 끝까지" 라는 뜻이라 len > 0 일 때만 부른다.
 * 소켓이 TCP 가 아니라(socketpair 등) 지원 안 하면, 아무것도 안 보낸 상태일 때 pread 경로로 넘긴다. */
static int darwin_sendfile(int sock, struct iovec *head, int file_fd, off_t offset, size_t len, send_clock *c)
{
    int saved;
    if (nonblock_begin(sock, &saved) < 0)
        return -1;
    int rc = 0, any_sent = 0, fallback = 0;
    while (len > 0) {
        struct sf_hdtr hdtr;                    /* 파일 앞(headers)·뒤(trailers)에 붙일 메모리 덩어리 */
        memset(&hdtr, 0, sizeof(hdtr));
        hdtr.headers = head;                    /* 남은 머리 (다 보냈으면 길이 0 → 아래에서 hdtr 자체를 안 넘김) */
        hdtr.hdr_cnt = 1;
        off_t sbytes = (off_t)len;              /* 입력: 보낼 파일 바이트 수 / 출력: 머리 포함 실제로 보낸 바이트 */
        int r = sendfile(file_fd, sock, offset, &sbytes, head->iov_len > 0 ? &hdtr : NULL, 0);
        int err = errno;                        /* 아래 계산이 errno 를 건드리지 않지만 명시적으로 보관 */

        size_t sent = sbytes > 0 ? (size_t)sbytes : 0;              /* 이번에 나간 총량 */
        size_t h = sent < head->iov_len ? sent : head->iov_len;     /* 그중 머리 몫 */
        head->iov_base = (char *)head->iov_base + h;                /* 머리 진행 */
        head->iov_len -= h;
        sent -= h;                                                  /* 나머지는 파일 몫 */
        offset += (off_t)sent;
        len -= sent;
        if (h > 0 || sent > 0) {
            any_sent = 1;
            clock_progress(c, h + sent);
        }

        if (r == 0) {                           /* 요청한 만큼 다 보냈거나 파일 끝(EOF) 에 닿음 */
            if (h == 0 && sent == 0) {          /* 하나도 못 보냈는데 성공 = 파일이 len 보다 짧음 */
                errno = EIO;
                rc = -1;
                break;
            }
            continue;                           /* len 이 남았으면 한 번 더 (짧은 파일이면 다음 호출이 0 바이트 → EIO) */
        }
        if (err == EINTR)
            continue;
        if (err == EAGAIN || err == EWOULDBLOCK) { /* 송신 버퍼 꽉 참 (일부는 나갔을 수 있음) */
            if (clock_wait_writable(sock, c) <= 0) {
                rc = -1;
                break;
            }
            continue;
        }
        if (!any_sent && (err == ENOTSOCK || err == ENOTSUP || err == EOPNOTSUPP || err == EINVAL)) {
            fallback = 1;                       /* 이 소켓 종류는 sendfile 불가 → 이식용 경로로 */
            break;
        }
        errno = err;                            /* EPIPE 등 */
        rc = -1;
        break;
    }
    nonblock_end(sock, saved);
    if (fallback)
        return pread_send(sock, head, file_fd, offset, len, c);
    return rc;
}
#endif

int tcp_sendfile_all(int sock, const void *head, size_t head_len,
                     int file_fd, off_t offset, size_t len, int timeout_ms) /* 머리 + 파일 구간 전송 */
{
    struct iovec h = {.iov_base = (void *)head, .iov_len = head ? head_len : 0}; /* 응답 머리 */
    if (len == 0)                                                       /* 파일 구간이 비었으면 머리만 */
        return sendv_until(sock, &h, 1, 0, now_ms() + timeout_ms);
    send_clock c;
    clock_begin(&c, timeout_ms);
#if defined(USE_LINUX_SENDFILE)
    /* 머리는 MSG_MORE("곧 더 보냄") 로 커널에 맡겨 두고 바로 보내지 않는다 → 이어지는 sendfile 데이터와
     * 같은 패킷으로 합쳐 나간다. 이렇게 안 하면 작은 머리 패킷이 따로 나가고, 파일 끝의 작은 조각이
     * Nagle 때문에 머리의 ACK(지연 ACK 최대 40ms) 를 기다리는 지연이 생길 수 있다.
     * sendfile 의 마지막 조각이 MSG_MORE 없이 나가면서 모아 둔 것을 한꺼번에 밀어낸다. */
    if (sendv_until(sock, &h, 1, MSG_MORE, c.start + timeout_ms) < 0)
        return -1;
    return linux_sendfile(sock, file_fd, offset, len, &c);
#elif defined(USE_DARWIN_SENDFILE)
    return darwin_sendfile(sock, &h, file_fd, offset, len, &c); /* 머리 동봉 sendfile */
#else
    return pread_send(sock, &h, file_fd, offset, len, &c);       /* 머리 + 첫 조각을 sendmsg 하나로 */
#endif
}

/* buf[from, to) 에서 "\r\n\r\n" 을 찾는다. 있으면 빈 줄까지 포함한 헤더 길이, 없으면 0.
 * strstr 대신 길이 기반: 이전 요청이 남긴 바이트에 '\0' 이 섞여 있어도 검색이 중간에 멈추지 않게 */
static size_t find_header_end(const char *buf, size_t from, size_t to)
{
    for (size_t i = from; i + 4 <= to; i++)                     /* 4바이트 창을 한 칸씩 민다 */
        if (memcmp(buf + i, "\r\n\r\n", 4) == 0)                /* 빈 줄(CRLF 두 번) */
            return i + 4;                                       /* 빈 줄까지 포함한 길이 */
    return 0;                                                   /* 못 찾음 */
}

ssize_t tcp_recv_request(int fd, char *buf, size_t cap, size_t used, int timeout_ms, size_t *header_len) /* HTTP 헤더 끝까지 받기 */
{
    *header_len = 0;                  /* 기본값: 헤더 끝을 아직 못 찾음 */
    if (cap == 0 || used > cap - 1) { /* NUL 자리조차 없음 / 이미 받은 양이 버퍼를 넘음 (호출자 버그) */
        errno = EINVAL;               /* 잘못된 인자 */
        return -1;
    }

    /* keep-alive 파이프라이닝: 이전 요청 뒤에 딸려 온 바이트가 buf[0..used) 에 이미 있다.
     * 그 안에 다음 요청 헤더가 통째로 있으면 시스템콜 없이 바로 돌려준다 (상대가 더 안 보내도 기다리지 않음) */
    buf[used] = '\0';                                  /* 문자열로도 안전하게 (잔여가 없으면 빈 문자열) */
    size_t end = find_header_end(buf, 0, used);        /* 잔여 전체에서 헤더 끝 찾기 */
    if (end != 0) {                                    /* 이미 다 와 있음 */
        *header_len = end;                             /* 빈 줄까지 포함한 헤더 길이 */
        return (ssize_t)used;                          /* 수신 없이 반환 */
    }
    if (used >= cap - 1)                               /* 이미 꽉 찼는데 헤더 끝이 없음 */
        return (ssize_t)used;                          /* header_len 0 → 호출자가 431 */

    long long deadline = now_ms() + timeout_ms; /* 요청 수신 전체의 마감 시각 */

    /* TCP 는 바이트 스트림이라 한 번의 recv 로 요청이 다 온다는 보장이 없다.
     * 헤더 끝 표시(빈 줄)가 보일 때까지 누적해서 읽되, 전체 마감 시간을 지킨다. */
    while (used < cap - 1) {                        /* 버퍼에 자리가 남은 동안 (마지막 1바이트는 '\0' 자리) */
        if (wait_until(fd, POLLIN, deadline) <= 0)  /* 읽을 데이터가 올 때까지 마감 시간 안에서 기다림 */
            return -1;                              /* 시간 초과 또는 오류 */
        ssize_t n = recv(fd, buf + used, cap - 1 - used, MSG_DONTWAIT); /* 커널 수신 버퍼 → buf 의 빈 곳으로 복사. 남은 자리만큼만 */
        if (n < 0) {                                /* 실패한 경우 */
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) /* 시그널 / 지금은 데이터 없음 → 일시적 */
                continue;                           /* 다시 기다림 */
            return -1;                              /* ECONNRESET 등 진짜 오류 */
        }
        if (n == 0) /* 상대가 연결을 닫음 */
            break;  /* 받은 데이터까지만 가지고 나감 (헤더 끝을 못 찾았으면 header_len 은 0) */

        /* 새로 받은 구간만 검사한다. 단 "\r\n\r\n" 이 이전 데이터(잔여 포함)와 걸칠 수
         * 있으므로 3바이트 앞부터. (매번 처음부터 찾으면 O(n^2)) */
        size_t scan_from = used >= 3 ? used - 3 : 0;  /* 검색 시작 위치: 직전까지 받은 끝에서 3바이트 앞 (앞쪽이 3바이트 미만이면 0) */
        used += (size_t)n;                            /* 받은 바이트 수 누적 */
        buf[used] = '\0';                             /* 문자열 끝 표시 → 호출자가 문자열 함수를 써도 안전 */
        end = find_header_end(buf, scan_from, used);  /* 헤더 끝 = 빈 줄(CRLF 두 번)을 찾음 */
        if (end != 0) {                               /* 찾았으면 */
            *header_len = end;                        /* 빈 줄까지 포함한 헤더 길이 */
            break;                                    /* 헤더를 다 받았으니 수신 종료 */
        }
    }
    return (ssize_t)used; /* buf 의 총 바이트 수 (잔여 포함. 헤더 뒤 본문·다음 요청 일부가 섞여 있을 수 있음) */
}

int tcp_recv_exact(int fd, void *buf, size_t len, int timeout_ms) /* 정확히 len 바이트를 받을 때까지 반복 수신 */
{
    char *p = buf;                              /* 아직 안 채운 부분의 시작 위치. void* 는 산술 연산이 안 돼서 char* 로 받음 */
    long long deadline = now_ms() + timeout_ms; /* 이 호출 전체의 마감 시각 (slowloris 대비) */
    while (len > 0) {                           /* 남은 바이트가 있는 동안 (recv 는 일부만 줄 수 있으므로) */
        if (wait_until(fd, POLLIN, deadline) <= 0) /* 데이터가 올 때까지 마감 시간 안에서 기다림 */
            return -1;                          /* 시간 초과(errno=ETIMEDOUT) 또는 오류 */
        ssize_t n = recv(fd, p, len, MSG_DONTWAIT); /* 커널 수신 버퍼 → p 로 복사. 남은 길이만큼만 (다음 요청을 먹지 않게) */
        if (n < 0) {                            /* 실패한 경우 */
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) /* 시그널 / 지금은 데이터 없음 → 일시적 */
                continue;                       /* 다시 기다림 */
            return -1;                          /* ECONNRESET 등 진짜 오류 */
        }
        if (n == 0) {                           /* 다 받기 전에 상대가 연결을 닫음 */
            errno = ECONNRESET;                 /* 호출자가 "도중에 끊김" 으로 알 수 있게 */
            return -1;                          /* 실패 */
        }
        p += n;           /* 받은 만큼 위치를 앞으로 */
        len -= (size_t)n; /* 남은 길이 감소 */
    }
    return 0; /* 전부 받음 */
}

ssize_t tcp_recv_some(int fd, void *buf, size_t cap, int timeout_ms) /* 와 있는 만큼 받기 (없으면 마감까지 기다림) */
{
    long long deadline = now_ms() + timeout_ms;          /* 이 호출의 마감 시각 */
    for (;;) {
        ssize_t n = recv(fd, buf, cap, MSG_DONTWAIT);    /* 먼저 시도 (데이터가 보통 이미 와 있음 → poll 시스템콜 절약) */
        if (n >= 0)                                      /* 받음(>0) 또는 상대가 닫음(0) */
            return n;
        if (errno == EINTR)                              /* 시그널 → 재시도 */
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)     /* ECONNRESET 등 진짜 오류 */
            return -1;
        if (wait_until(fd, POLLIN, deadline) <= 0)       /* 아직 없음 → 마감 안에서 기다림 */
            return -1;                                   /* 시간 초과(errno=ETIMEDOUT) 또는 오류 */
    }
}

int tcp_connect(const struct sockaddr_in *addr, int timeout_ms) /* 마감 시간이 있는 connect */
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);            /* IPv4 TCP 소켓 */
    if (fd < 0)
        return -1;
    int fl = fcntl(fd, F_GETFL);                         /* 논블로킹 + CLOEXEC (accept4 가 없는 곳과 같은 방식) */
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    if (connect(fd, (const struct sockaddr *)addr, sizeof(*addr)) == 0) /* 루프백은 바로 성공하기도 함 */
        return fd;
    if (errno != EINPROGRESS && errno != EINTR) {        /* 즉시 실패 (ECONNREFUSED, ENETUNREACH ...) */
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    /* 3-way handshake 진행 중: 쓰기 가능해지면 끝난 것 (성공/실패는 SO_ERROR 로 확인) */
    if (wait_until(fd, POLLOUT, now_ms() + timeout_ms) <= 0) {
        int e = errno;                                   /* ETIMEDOUT: SYN 에 답이 없음 (꺼진 호스트, 방화벽) */
        close(fd);
        errno = e;
        return -1;
    }
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
        err = errno;
    if (err != 0) {                                      /* handshake 실패 (ECONNREFUSED 등) */
        close(fd);
        errno = err;
        return -1;
    }
    return fd;                                           /* 연결됨 */
}

#define DRAIN_TIMEOUT_MS 500          /* 남은 데이터 비우기에 쓸 수 있는 최대 시간 (전체 기준) */
#define DRAIN_MAX_BYTES (64 * 1024)   /* 비우기로 읽어 버릴 최대 바이트 수 */

void tcp_close_graceful(int fd) /* 응답을 보낸 뒤 RST 없이 연결 닫기 */
{
    /* 읽지 않은 수신 데이터가 없으면 그냥 close 해도 RST 가 나가지 않는다.
     * 이때는 상대의 FIN 을 기다릴 이유가 없으므로 바로 닫는다. */
    char peek;                                               /* 엿보기용 1바이트 */
    if (recv(fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT) <= 0) {  /* PEEK: 데이터를 큐에서 빼지 않고 보기만 / DONTWAIT: 없으면 바로 리턴. 0(FIN) 또는 -1(없음) */
        close(fd);                                           /* 남은 데이터가 없으니 즉시 닫음 → 정상 FIN */
        return;                                              /* 끝 */
    }

    /* 1. 더 보낼 게 없다고 알림(FIN). 상대는 응답을 다 읽고 연결을 닫는다. */
    shutdown(fd, SHUT_WR); /* 송신 방향만 닫음(half-close). 수신은 계속 가능 → 아래에서 남은 데이터를 읽을 수 있음 */

    /* 2. 남은 데이터를 읽어 버린다. 시간 상한은 호출당이 아니라 "전체" 기준이고,
     *    바이트 상한도 함께 둬서 악의적 클라이언트에 붙잡히지 않게 한다. */
    long long deadline = now_ms() + DRAIN_TIMEOUT_MS;  /* 비우기 전체의 마감 시각 */
    char junk[4096];                                   /* 읽어서 버릴 임시 버퍼 */
    size_t total = 0;                                  /* 지금까지 버린 바이트 수 */
    while (total < DRAIN_MAX_BYTES && wait_until(fd, POLLIN, deadline) > 0) { /* 바이트 상한 안 넘고, 마감 전에 읽을 게 있으면 반복 */
        ssize_t n = recv(fd, junk, sizeof(junk), MSG_DONTWAIT);           /* 남은 데이터를 읽어서 버림 */
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) /* 일시적 상황 */
            continue;                                                     /* 다시 기다림 */
        if (n <= 0) /* 상대가 닫음(FIN) 또는 오류 */
            break;  /* 더 읽을 필요 없음 */
        total += (size_t)n; /* 버린 양 누적 */
    }
    close(fd); /* 소켓 반납. 수신 버퍼가 비었으면 RST 대신 정상 종료 */
}
