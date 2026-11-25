/* idle_poller.c — keep-alive 로 쉬는 연결을 감시하는 스레드 하나.
 *
 *   idle_park (아무 스레드) ──[mutex 대기열 + wake pipe 1바이트]──▶ 감시 스레드가 꺼내 등록
 *   감시 스레드: epoll_wait(리눅스) / poll(그 밖) 로 "읽기 가능 / HUP / ERR" 만 본다 (읽지는 않음 → 다음 요청 바이트 보존)
 *     - 이벤트: 감시 목록에서 뺀 뒤(epoll DEL) 락 없이 ready_fn(job, ctx) 호출 → fd 소유권이 ready_fn 으로
 *     - 타임아웃: 모든 연결의 제한 시간이 같다 → park 순서 = 마감 순서.
 *       이중 연결 리스트(FIFO)로 삽입·삭제 O(1), 맨 앞만 보고 만료 처리. 대기 시간 = 맨 앞 마감까지 남은 시간.
 *
 * 리스트·epoll 등록은 감시 스레드만 만진다 (락 불필요). 다른 스레드와 공유하는 것은 대기열과 개수뿐 (mutex).
 * 백엔드: 리눅스는 epoll, 그 밖(또는 -DIDLE_FORCE_POLL)은 poll — poll 은 매 루프 pollfd 배열을 다시 만든다 (O(n)).
 *
 * 왜 다른 스레드가 epoll_ctl 을 직접 부르지 않고 대기열을 거치나:
 *   epoll_ctl 자체는 스레드 안전하지만, 감시 리스트(마감 순서)까지 여러 스레드가 만지면 락이 필요해지고
 *   감시 스레드가 이벤트를 처리하는 내내 그 락을 잡아야 한다. 대기열로 넘기면 리스트는 감시 스레드 혼자 쓴다.
 *   poll 백엔드는 어차피 감시 스레드가 배열을 만들어야 하므로 같은 구조로 통일된다.
 *
 * 종료와 동시 호출 (idle_stop 이 free 하는 순간 다른 스레드가 idle_park 안에 있으면 안 됨):
 *   - stopping 은 mu 안에서 켠다 → mu 안에서 stopping 을 다시 보는 idle_park 는 확실히 거절된다.
 *   - inflight 카운터: idle_park / idle_count 가 들어올 때 +1, 나갈 때 -1. idle_stop 은 0 이 될 때까지 free 를 미룬다.
 *   - wake pipe 에 쓰는 것은 항상 mu 안 → idle_stop 이 pipe 를 닫는 시점(inflight 0 이후)에는 아무도 쓰지 않는다. */

#define _POSIX_C_SOURCE 200809L /* clock_gettime, pthread_sigmask, nanosleep 등을 헤더에서 보이게 함. include 보다 먼저 */

#if defined(__linux__) && !defined(IDLE_FORCE_POLL) /* 리눅스이고 poll 강제(테스트용)가 아니면 */
#define IDLE_USE_EPOLL 1 /* 리눅스: epoll (연결 수와 무관하게 깨어난 것만 처리) */
#endif

#include "idle_poller.h" /* 이 파일이 구현할 선언 */

#include <errno.h>      /* errno, EINVAL, EAGAIN, ECANCELED, EINTR */
#include <fcntl.h>      /* fcntl, O_NONBLOCK, FD_CLOEXEC (wake pipe 설정) */
#include <pthread.h>    /* pthread_create/join, mutex */
#include <signal.h>     /* sigset_t, pthread_sigmask, SIGINT, SIGTERM */
#include <stdatomic.h>  /* atomic_int (stopping, inflight) */
#include <stdio.h>      /* fprintf, perror */
#include <stdlib.h>     /* malloc, calloc, free */
#include <string.h>     /* memset */
#include <time.h>       /* clock_gettime, CLOCK_MONOTONIC, nanosleep */
#include <unistd.h>     /* pipe, read, write, close */
#if defined(IDLE_USE_EPOLL)
#include <sys/epoll.h>  /* epoll_create1, epoll_ctl, epoll_wait, EPOLLIN/EPOLLRDHUP */
#else
#include <poll.h>       /* poll, struct pollfd, POLLIN */
#endif

#define IDLE_MAX_EVENTS 256 /* epoll_wait 한 번에 받을 최대 이벤트 수 (남은 건 다음 호출에서 받음) */

/* 감시 중(또는 대기열에 있는) 연결 하나 */
typedef struct idle_entry { /* 연결 하나의 항목 (malloc 으로 하나씩) */
    conn_job job;            /* park 때 받은 job 복사본 (fd 소유권 포함) */
    long long deadline_ns;   /* 이 시각(단조 시계, ns)까지 조용하면 close */
    struct idle_entry *prev; /* 감시 리스트 이전 (대기열에서는 안 씀) */
    struct idle_entry *next; /* 감시 리스트 다음 / 대기열 다음 */
} idle_entry;                /* 같은 항목이 대기열 → 감시 리스트로 옮겨 가므로 링크 필드를 함께 씀 */

struct idle_poller { /* poller 전체 상태 (헤더에는 이름만 보이는 불투명 타입) */
    int timeout_ms;      /* 쉬는 연결을 닫기까지의 시간 */
    size_t max_conns;    /* (감시 중 + 대기열) 상한 */
    idle_ready_fn fn;    /* 준비된 연결을 넘길 콜백 */
    void *ctx;           /* fn 에 그대로 넘길 값 */

    pthread_t thread;    /* 감시 스레드 */
    pthread_mutex_t mu;  /* 아래 "mu 보호" 필드를 지킨다 */
    idle_entry *pend_head; /* mu 보호: park 됐지만 아직 감시 스레드가 등록 안 한 것 (단일 연결 FIFO) */
    idle_entry *pend_tail; /* mu 보호: 대기열 끝 (O(1) 추가) */
    size_t npend;          /* mu 보호: 대기열 길이 */
    size_t nwatch;         /* mu 보호: 감시 리스트 길이 (max_conns·idle_count 계산용) */

    atomic_int stopping; /* 1 이면 종료 중 (mu 안에서 켜고, 감시 스레드는 락 없이 읽음) */
    atomic_int inflight; /* 지금 idle_park/idle_count 안에 있는 호출 수 → idle_stop 이 0 될 때까지 free 를 미룸 */

    int wake_r;          /* wake pipe 읽는 쪽 (감시 스레드가 감시) */
    int wake_w;          /* wake pipe 쓰는 쪽 (idle_park/idle_stop 이 1바이트) */

    idle_entry *head;    /* 감시 스레드 전용: 감시 리스트 맨 앞 (마감이 가장 이름) */
    idle_entry *tail;    /* 감시 스레드 전용: 맨 뒤 (가장 최근 park) */

#if defined(IDLE_USE_EPOLL)
    int epfd;                  /* epoll 인스턴스 */
#else
    struct pollfd *pfds;       /* poll 배열: [0] = wake pipe, [1..] = 감시 중 연결 */
    idle_entry **pents;        /* pfds[i] 에 대응하는 항목 (pfds 와 나란히) */
#endif
};

static long long mono_ns(void) /* 단조 시계 현재 시각 (ns). 시스템 시간 변경에 영향 없음 */
{
    struct timespec ts;                                        /* 초(tv_sec) + 나노초(tv_nsec) */
    clock_gettime(CLOCK_MONOTONIC, &ts);                       /* MONOTONIC: 부팅 후 계속 증가 (사용자가 시계를 바꿔도 거꾸로 안 감) */
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;   /* 초 → ns 로 바꿔 더함. long long 으로 곱해 넘침 방지 */
}

static void sleep_ms(long ms) /* 오류 뒤 헛돌지 않게 잠깐 쉼 */
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L}; /* ms → 초 + 나노초 */
    nanosleep(&ts, NULL);                                      /* 시그널로 일찍 깨어나도 상관없음 (그냥 조금 덜 쉼) */
}

static int set_nb_cloexec(int fd) /* fd 를 논블로킹 + exec 시 자동 close 로. 성공 0, 실패 -1 (errno) */
{
    int fl = fcntl(fd, F_GETFL);                                  /* 현재 상태 플래그 */
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)       /* 논블로킹: 가득 찬 pipe 에 써도 막히지 않음 */
        return -1;                                                /* 실패 (errno 는 fcntl 이 설정) */
    int fdfl = fcntl(fd, F_GETFD);                                /* fd 플래그 */
    if (fdfl < 0 || fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC) < 0)   /* CGI 등 exec 한 자식에게 새지 않게 */
        return -1;                                                /* 실패 */
    return 0;                                                     /* 성공 */
}

static void wake(idle_poller *ip) /* 감시 스레드를 깨운다 (mu 를 잡은 채 호출 → idle_stop 이 pipe 를 닫기 전임이 보장됨) */
{
    char b = 1;                                 /* 보낼 1바이트 (값은 의미 없음) */
    ssize_t w;                                  /* write 결과 */
    do { /* write 를 EINTR 이면 반복 */
        w = write(ip->wake_w, &b, 1);           /* 1바이트면 충분: 감시 스레드의 epoll_wait/poll 이 "읽기 가능" 으로 깨어남 */
    } while (w < 0 && errno == EINTR);          /* 시그널에 끊기면 다시 */
    /* EAGAIN(가득) 이면 이미 깨울 바이트가 쌓여 있으니 무시 */
}

static void drain_wake(idle_poller *ip) /* wake pipe 에 쌓인 바이트를 모두 비움 (레벨 트리거라 안 비우면 계속 깨어남) */
{
    char buf[64];                               /* 읽어서 버릴 임시 버퍼 */
    for (;;) {                                  /* 빌 때까지 */
        ssize_t r = read(ip->wake_r, buf, sizeof(buf)); /* 논블로킹이라 비었으면 바로 EAGAIN */
        if (r > 0)                              /* 읽었으면 */
            continue;                           /* 더 있을 수 있음 */
        if (r < 0 && errno == EINTR)            /* 시그널에 끊김 */
            continue;                           /* 재시도 */
        break;                                  /* EAGAIN(비었음) 또는 오류 → 끝 */
    }
}

/* ---- 감시 리스트 (감시 스레드 전용, 락 없음) ---- */

static void list_append(idle_poller *ip, idle_entry *e) /* 맨 뒤에 붙임 = 마감이 가장 늦음 */
{
    e->next = NULL;                             /* 맨 뒤라 다음 없음 */
    e->prev = ip->tail;                         /* 이전 = 지금의 맨 뒤 */
    if (ip->tail)                               /* 리스트가 비어 있지 않으면 */
        ip->tail->next = e;                     /* 기존 맨 뒤 다음에 연결 */
    else                                        /* 비어 있었으면 */
        ip->head = e;                           /* 맨 앞도 이것 */
    ip->tail = e;                               /* 새 맨 뒤 */
}

static void list_remove(idle_poller *ip, idle_entry *e) /* 아무 위치에서나 O(1) 로 뺌 */
{
    if (e->prev)                                /* 앞이 있으면 */
        e->prev->next = e->next;                /* 앞이 내 다음을 가리키게 */
    else                                        /* 내가 맨 앞이었으면 */
        ip->head = e->next;                     /* 다음이 새 맨 앞 */
    if (e->next)                                /* 뒤가 있으면 */
        e->next->prev = e->prev;                /* 뒤가 내 앞을 가리키게 */
    else                                        /* 내가 맨 뒤였으면 */
        ip->tail = e->prev;                     /* 앞이 새 맨 뒤 */
    e->prev = e->next = NULL;                   /* 떨어져 나온 항목의 링크 정리 (실수로 따라가지 않게) */
}

static void watch_dec(idle_poller *ip) /* 감시 리스트에서 하나 빠짐 → 개수 감소 (park 자리 생김) */
{
    pthread_mutex_lock(&ip->mu);                /* nwatch 는 idle_park 가 다른 스레드에서 읽으므로 mu 보호 */
    ip->nwatch--;                               /* 하나 감소 */
    pthread_mutex_unlock(&ip->mu);              /* 바로 놓음 */
}

static void backend_del(idle_poller *ip, idle_entry *e) /* 커널 감시 목록에서 제거 (poll 은 다음 배열 재구성에서 빠지므로 할 일 없음) */
{
#if defined(IDLE_USE_EPOLL)
    epoll_ctl(ip->epfd, EPOLL_CTL_DEL, e->job.fd, NULL); /* close 전에 빼야 다른 곳에서 재사용된 fd 번호와 섞이지 않음 */
#else
    (void)ip;                                   /* 안 쓰는 인자 경고 제거 */
    (void)e;                                    /* 〃 */
#endif
}

/* 이벤트가 온 연결: 목록에서 빼고(DEL) → 개수 감소 → 락 없이 ready_fn */
static void handle_ready(idle_poller *ip, idle_entry *e) /* e: 이벤트가 온 항목 (호출 뒤 사라짐) */
{
    backend_del(ip, e);                         /* 먼저 감시 해제 (ready_fn 이후 fd 는 남의 것) */
    list_remove(ip, e);                         /* 마감 리스트에서도 빼서 만료 처리 대상에서 제외 */
    conn_job job = e->job;                      /* 스택에 복사 → 항목은 바로 반납 */
    free(e);                                    /* 항목 메모리 반납 */
    watch_dec(ip);                              /* ready_fn 안에서 다시 park 할 자리를 먼저 비워 둠 */
    ip->fn(&job, ip->ctx);                      /* 락 없이 호출: 안에서 idle_park 해도 교착 없음 */
}

/* 대기열을 통째로 가져와 감시 리스트에 등록 */
static void take_pending(idle_poller *ip) /* 감시 스레드에서만 호출 */
{
    pthread_mutex_lock(&ip->mu);                /* 대기열은 다른 스레드와 공유 → 잠깐만 잡고 */
    idle_entry *e = ip->pend_head;              /* 목록 전체를 가져감 */
    ip->nwatch += ip->npend;                    /* 개수는 "대기열 → 감시" 로 옮김 (합은 그대로) */
    ip->pend_head = ip->pend_tail = NULL;       /* 대기열 비움 */
    ip->npend = 0;                              /* 대기열 길이 0 */
    pthread_mutex_unlock(&ip->mu);              /* 등록(시스템콜)은 락 밖에서 → idle_park 가 오래 안 기다림 */

    while (e) {                                 /* 가져온 항목을 하나씩 */
        idle_entry *next = e->next;             /* 아래에서 e 를 free 하거나 링크를 바꾸므로 다음을 먼저 기억 */
#if defined(IDLE_USE_EPOLL)
        struct epoll_event ev;                  /* 커널에 등록할 감시 조건 */
        memset(&ev, 0, sizeof(ev));             /* 안 쓰는 필드 0 */
        ev.events = EPOLLIN | EPOLLRDHUP;       /* 읽기 가능 + 상대 FIN. EPOLLHUP/EPOLLERR 는 항상 보고됨 */
        ev.data.ptr = e;                        /* 이벤트에서 바로 항목을 찾기 위해 */
        if (epoll_ctl(ip->epfd, EPOLL_CTL_ADD, e->job.fd, &ev) < 0) { /* 등록 실패 (메모리 부족, 잘못된 fd 등) */
            perror("idle_poller: epoll_ctl(ADD)"); /* 이유 로그 */
            close(e->job.fd);                   /* 소유권은 이미 poller → 감시 못 하면 닫는다 */
            free(e);                            /* 항목 반납 */
            watch_dec(ip);                      /* 위에서 nwatch 에 더해 둔 것 취소 */
            e = next;                           /* 다음 항목으로 */
            continue;                           /* 리스트에는 안 붙임 */
        }
#endif
        list_append(ip, e);                     /* 대기열이 park 순서(= 마감 순서)라 뒤에 붙여도 정렬 유지 */
        e = next;                               /* 다음 항목 */
    }
}

/* 맨 앞부터 마감이 지난 연결을 닫는다. 반환: 다음 대기 시간(ms), 감시 중인 게 없으면 -1 */
static int expire(idle_poller *ip) /* 감시 스레드에서만 호출 */
{
    long long now = mono_ns();                  /* 지금 시각 한 번만 읽음 */
    while (ip->head && ip->head->deadline_ns <= now) { /* 맨 앞이 안 지났으면 뒤는 모두 안 지남 */
        idle_entry *e = ip->head;               /* 마감 지난 맨 앞 */
        backend_del(ip, e);                     /* close 전에 감시 해제 */
        list_remove(ip, e);                     /* 리스트에서 뺌 */
        close(e->job.fd);                       /* 진행 중인 요청이 없으니 그냥 닫음 */
        free(e);                                /* 항목 반납 */
        watch_dec(ip);                          /* 개수 감소 */
    }
    if (!ip->head)                              /* 감시할 게 없음 */
        return -1;                              /* → 깨울 때까지 무한 대기 */
    long long left = ip->head->deadline_ns - now;  /* 맨 앞 마감까지 남은 ns (> 0) */
    long long ms = (left + 999999) / 1000000;      /* 올림: 내림하면 마감 직전에 깨어 헛돈다 */
    return ms > 1000000000LL ? 1000000000 : (int)ms; /* int 범위 안으로 (사실상 timeout_ms 이하) */
}

static void *poller_main(void *arg) /* 감시 스레드 본체 */
{
    idle_poller *ip = arg;                      /* pthread_create 가 넘겨준 poller */
#if defined(IDLE_USE_EPOLL)
    struct epoll_event evs[IDLE_MAX_EVENTS];    /* epoll_wait 가 채울 이벤트 배열 */
#endif

    while (!atomic_load(&ip->stopping)) {       /* 종료 신호까지 반복 */
        int timeout = expire(ip);               /* 만료 처리 + 다음 마감까지 대기 시간 */
#if defined(IDLE_USE_EPOLL)
        int n = epoll_wait(ip->epfd, evs, IDLE_MAX_EVENTS, timeout); /* 준비된 fd 가 생기거나 timeout 까지 잠듦 */
        if (n < 0) {                            /* 실패 */
            if (errno == EINTR)                 /* 시그널에 끊김 */
                continue;                       /* 재시도 */
            perror("idle_poller: epoll_wait");  /* 그 밖의 오류 로그 */
            sleep_ms(10);                       /* 같은 오류로 헛돌지 않게 잠깐 쉼 */
            continue;                           /* 다시 시도 */
        }
        int woke = 0;                           /* wake pipe 가 울렸는지 */
        for (int i = 0; i < n; i++) {           /* 받은 이벤트마다 */
            if (evs[i].data.ptr == NULL)        /* NULL = wake pipe 표시 */
                woke = 1;                       /* 대기열 처리는 루프 뒤에서 한 번만 */
            else                                /* 연결 항목 */
                handle_ready(ip, evs[i].data.ptr); /* 연결 준비됨 → 목록에서 빼고 ready_fn 으로 */
        }
#else
        /* poll: 매번 [wake pipe] + 감시 리스트 순서로 배열을 다시 만든다 (O(n), 크기 ≤ max_conns + 1) */
        nfds_t nfds = 1;                        /* [0] 은 wake pipe 자리 */
        ip->pfds[0].fd = ip->wake_r;            /* wake pipe 읽는 쪽 */
        ip->pfds[0].events = POLLIN;            /* 바이트가 들어오면 깨어남 */
        ip->pfds[0].revents = 0;                /* 결과 칸 초기화 */
        for (idle_entry *e = ip->head; e; e = e->next) { /* 감시 리스트 전체 */
            ip->pfds[nfds].fd = e->job.fd;      /* 이 연결의 fd */
            ip->pfds[nfds].events = POLLIN;     /* HUP/ERR/NVAL 은 revents 에 항상 보고됨 */
            ip->pfds[nfds].revents = 0;         /* 결과 칸 초기화 */
            ip->pents[nfds] = e;                /* 같은 번호에 항목 포인터 → 이벤트에서 바로 찾음 */
            nfds++;                             /* 다음 칸 */
        }
        int n = poll(ip->pfds, nfds, timeout);  /* 준비된 fd 수, 0 = 시간 초과, -1 = 오류 */
        if (n < 0) {                            /* 실패 */
            if (errno == EINTR)                 /* 시그널에 끊김 */
                continue;                       /* 재시도 */
            perror("idle_poller: poll");        /* 그 밖의 오류 로그 */
            sleep_ms(10);                       /* 헛돌지 않게 */
            continue;                           /* 다시 시도 */
        }
        int woke = ip->pfds[0].revents != 0;    /* wake pipe 가 울렸는지 */
        for (nfds_t i = 1; i < nfds && n > 0; i++) { /* 연결 칸만 (n == 0 이면 볼 것 없음) */
            if (ip->pfds[i].revents) {          /* 읽기 가능 / HUP / ERR / NVAL 모두 "준비" */
                handle_ready(ip, ip->pents[i]); /* 리스트에서 빠지면 다음 배열 재구성에서 제외됨 */
            }
        }
#endif
        if (woke) {                             /* 다른 스레드가 park 했거나 idle_stop 이 깨움 */
            drain_wake(ip);                     /* 먼저 비우고 → 그다음 대기열을 가져감 (그 사이 들어온 park 는 새 바이트로 다시 깨움) */
            take_pending(ip);                   /* 대기열 → 감시 리스트 (종료 중이면 루프 조건에서 빠져나감) */
        }
    }
    return NULL;                                /* 남은 연결 정리는 idle_stop 이 join 뒤에 한다 */
}

/* poller 생성: 인자 검사 → mutex → wake pipe → epoll(또는 poll 배열) → 감시 스레드.
 * 중간에 실패하면 그때까지 만든 것을 모두 정리하고 NULL (errno 는 처음 실패한 이유) */
idle_poller *idle_create(int idle_timeout_ms, size_t max_conns, idle_ready_fn fn, void *ctx) /* idle_timeout_ms: 쉬는 시간 상한, max_conns: 감시 수 상한, fn/ctx: 준비 콜백 */
{
    if (idle_timeout_ms <= 0 || max_conns == 0 || fn == NULL) { /* 인자 검사 */
        errno = EINVAL;                         /* 잘못된 인자 */
        return NULL;                            /* 아무것도 안 만듦 */
    }

    idle_poller *ip = calloc(1, sizeof(*ip));   /* 0 으로 채운 구조체 (포인터 NULL, 개수 0) */
    if (!ip)                                    /* 할당 실패 */
        return NULL;                            /* errno = ENOMEM (calloc 이 설정) */
    ip->timeout_ms = idle_timeout_ms;           /* 쉬는 시간 상한 */
    ip->max_conns = max_conns;                  /* 감시 수 상한 */
    ip->fn = fn;                                /* 준비 콜백 */
    ip->ctx = ctx;                              /* 콜백에 넘길 값 */
    ip->wake_r = ip->wake_w = -1;               /* -1 = 아직 안 엶 (실패 정리에서 구분) */
    atomic_init(&ip->stopping, 0);              /* 실행 중 */
    atomic_init(&ip->inflight, 0);              /* 진행 중인 호출 없음 */
#if defined(IDLE_USE_EPOLL)
    ip->epfd = -1;                              /* 아직 안 만듦 */
#endif
    int err = 0;                                /* 실패 시 돌려줄 errno */

    if ((err = pthread_mutex_init(&ip->mu, NULL)) != 0) { /* pthread 함수는 errno 대신 오류 번호를 반환 */
        free(ip);                               /* 구조체만 있으니 그것만 반납 */
        errno = err;                            /* 실패 이유 전달 */
        return NULL;                            /* 실패 */
    }

    int p[2];                                   /* pipe 양 끝: [0] 읽기, [1] 쓰기 */
    if (pipe(p) < 0) {                          /* 이식성: pipe2/eventfd 대신 pipe + fcntl */
        err = errno;                            /* 실패 이유 보관 (정리 중 errno 가 바뀌므로) */
        goto fail;                              /* 정리로 */
    }
    ip->wake_r = p[0];                          /* 감시 스레드가 감시할 쪽 */
    ip->wake_w = p[1];                          /* 깨우는 쪽 */
    if (set_nb_cloexec(ip->wake_r) < 0 || set_nb_cloexec(ip->wake_w) < 0) { /* 양쪽 다 논블로킹 + CLOEXEC */
        err = errno;                            /* 실패 이유 보관 */
        goto fail;                              /* 정리로 */
    }

#if defined(IDLE_USE_EPOLL)
    ip->epfd = epoll_create1(EPOLL_CLOEXEC);    /* epoll 인스턴스 (exec 시 자동 close) */
    if (ip->epfd < 0) {                         /* 생성 실패 (fd 고갈 등) */
        err = errno;                            /* 실패 이유 보관 */
        goto fail;                              /* 정리로 */
    }
    struct epoll_event ev;                      /* wake pipe 감시 조건 */
    memset(&ev, 0, sizeof(ev));                 /* 안 쓰는 필드 0 */
    ev.events = EPOLLIN;                        /* 바이트가 들어오면 */
    ev.data.ptr = NULL;                         /* NULL = wake pipe (연결 항목은 항상 NULL 아님) */
    if (epoll_ctl(ip->epfd, EPOLL_CTL_ADD, ip->wake_r, &ev) < 0) { /* 처음부터 끝까지 등록된 채로 둠 */
        err = errno;                            /* 실패 이유 보관 */
        goto fail;                              /* 정리로 */
    }
#else
    if (max_conns > ((size_t)-1) / sizeof(struct pollfd) - 1) { /* (max_conns + 1) * 크기 가 size_t 를 넘는지 */
        err = ENOMEM;                           /* 그만큼은 할당할 수 없음 */
        goto fail;                              /* 정리로 */
    }
    ip->pfds = calloc(max_conns + 1, sizeof(*ip->pfds));  /* 감시 수는 max_conns 를 넘지 않음 (+1 = wake pipe) */
    ip->pents = calloc(max_conns + 1, sizeof(*ip->pents)); /* pfds 와 같은 번호로 항목 포인터 */
    if (!ip->pfds || !ip->pents) {              /* 둘 중 하나라도 실패 (free(NULL) 은 안전) */
        err = ENOMEM;                           /* 메모리 부족 */
        goto fail;                              /* 정리로 */
    }
#endif

    /* 감시 스레드는 SIGINT/SIGTERM 을 받지 않게: 막은 채 만들면 새 스레드가 마스크를 물려받음 → 만든 뒤 원래대로 */
    sigset_t block, old;                        /* 막을 시그널 집합, 원래 마스크 */
    sigemptyset(&block);                        /* 빈 집합에서 시작 */
    sigaddset(&block, SIGINT);                  /* Ctrl+C */
    sigaddset(&block, SIGTERM);                 /* kill */
    pthread_sigmask(SIG_BLOCK, &block, &old);   /* 지금 스레드에서 잠깐 막음 (원래 마스크는 old 에) */
    err = pthread_create(&ip->thread, NULL, poller_main, ip); /* 감시 스레드 시작 (막힌 마스크를 물려받음) */
    pthread_sigmask(SIG_SETMASK, &old, NULL);   /* 호출한 스레드의 마스크 복원 */
    if (err != 0)                               /* 스레드 생성 실패 (EAGAIN 등) */
        goto fail;                              /* 정리로 (err 는 pthread_create 가 준 번호) */
    return ip;                                  /* 성공 */

fail:                                           /* 만든 자원 모두 정리 */
#if defined(IDLE_USE_EPOLL)
    if (ip->epfd >= 0)                          /* epoll 을 만들었으면 */
        close(ip->epfd);                        /* 닫기 */
#else
    free(ip->pfds);                             /* NULL 이어도 안전 */
    free(ip->pents);                            /* 〃 */
#endif
    if (ip->wake_r >= 0)                        /* pipe 를 열었으면 */
        close(ip->wake_r);                      /* 읽는 쪽 닫기 */
    if (ip->wake_w >= 0)                        /* 〃 */
        close(ip->wake_w);                      /* 쓰는 쪽 닫기 */
    pthread_mutex_destroy(&ip->mu);             /* mutex 정리 (여기 오면 init 은 성공한 상태) */
    free(ip);                                   /* 구조체 반납 */
    errno = err;                                /* 정리 중 바뀌었을 수 있는 errno 를 원래 실패 이유로 */
    return NULL;                                /* 실패 */
}

/* 연결 하나를 맡긴다 (아무 스레드에서나). 대기열에 넣고 감시 스레드를 깨울 뿐 기다리지 않는다.
 * 순서: inflight+1 → 빠른 거절 → (락 밖) 항목 할당 → (락 안) 최종 거절 판단 / 대기열 추가 / 깨움 → inflight-1
 * 반환: 0 = fd 소유권이 poller 로, -1 = 거절 (fd 는 호출자 것 그대로, errno = EINVAL/ECANCELED/EAGAIN/ENOMEM) */
int idle_park(idle_poller *ip, const conn_job *job) /* job: 맡길 연결 (복사됨) */
{
    if (!ip || !job || job->fd < 0) {           /* 잘못된 인자 */
        errno = EINVAL;                         /* 인자 오류 */
        return -1;                              /* 거절 */
    }
    atomic_fetch_add(&ip->inflight, 1);         /* idle_stop 이 이 호출이 끝나기 전에 free 하지 않게 */
    if (atomic_load(&ip->stopping)) {           /* 빠른 거절 (최종 판단은 아래 mu 안에서) */
        atomic_fetch_sub(&ip->inflight, 1);     /* 들어왔던 표시 취소 */
        errno = ECANCELED;                      /* 종료 중 */
        return -1;                              /* 거절 */
    }

    idle_entry *e = malloc(sizeof(*e));         /* 락 밖에서 할당 (락 잡는 시간 최소화) */
    if (!e) {                                   /* 메모리 부족 */
        atomic_fetch_sub(&ip->inflight, 1);     /* 들어왔던 표시 취소 */
        errno = ENOMEM;                         /* 메모리 부족 */
        return -1;                              /* 거절 */
    }
    e->job = *job;                              /* job 복사 (호출자의 job 은 스택이라 곧 사라짐) */
    e->prev = e->next = NULL;                   /* 아직 어느 목록에도 없음 */

    int rc = 0, err = 0;                        /* 결과와 실패 이유 (errno 는 unlock 뒤에 설정) */
    pthread_mutex_lock(&ip->mu);                /* 대기열·개수·stopping 판단을 한 덩어리로 */
    if (atomic_load(&ip->stopping)) {           /* idle_stop 은 mu 안에서 stopping 을 켬 → 여기서 보이면 확실히 거절 */
        err = ECANCELED;                        /* 종료 중 */
        rc = -1;                                /* 거절 */
    } else if (ip->npend + ip->nwatch >= ip->max_conns) { /* 감시 중 + 대기열 이 상한 */
        err = EAGAIN;                           /* 가득 참 (호출자가 close) */
        rc = -1;                                /* 거절 */
    } else {                                    /* 받아들임 */
        e->deadline_ns = mono_ns() + (long long)ip->timeout_ms * 1000000LL; /* mu 안에서 시각을 읽음 → 대기열 순서 = 마감 순서 */
        int was_empty = (ip->pend_head == NULL); /* 대기열이 비어 있었는지 (깨울지 판단) */
        if (ip->pend_tail)                      /* 대기열에 뭔가 있으면 */
            ip->pend_tail->next = e;            /* 끝 다음에 붙임 */
        else                                    /* 비어 있었으면 */
            ip->pend_head = e;                  /* 맨 앞도 이것 */
        ip->pend_tail = e;                      /* 새 끝 */
        ip->npend++;                            /* 대기열 길이 증가 */
        if (was_empty)                          /* 비어 있지 않았으면 앞선 park 의 깨움이 아직 처리 전 → 그때 함께 가져감 */
            wake(ip);                           /* mu 안에서 씀: idle_stop 이 pipe 를 닫기 전임이 보장됨 */
    }
    pthread_mutex_unlock(&ip->mu);              /* 락 해제 */

    if (rc < 0)                                 /* 거절됐으면 */
        free(e);                                /* 항목만 반납 (fd 는 건드리지 않음 — 소유권은 호출자) */
    atomic_fetch_sub(&ip->inflight, 1);         /* 이 뒤로 ip 를 만지지 않음 */
    if (rc < 0)                                 /* 거절됐으면 */
        errno = err;                            /* 실패 이유 전달 (free 등이 errno 를 바꿨을 수 있어서 마지막에) */
    return rc;                                  /* 0 = 맡김, -1 = 거절 */
}

/* 지금 쉬는 연결 수 (대기열 + 감시 리스트). 다른 스레드가 바꾸는 중일 수 있으니 근사값 */
size_t idle_count(idle_poller *ip) /* 반환: 대기열 + 감시 중 연결 수 */
{
    if (!ip)                                    /* 없는 poller */
        return 0;                               /* = 0 개 */
    atomic_fetch_add(&ip->inflight, 1);         /* idle_stop 이 free 하지 않게 */
    pthread_mutex_lock(&ip->mu);                /* 두 개수를 같은 순간에 읽기 위해 */
    size_t n = ip->npend + ip->nwatch;          /* 대기열에 있는 것도 "쉬는 연결" 로 셈 */
    pthread_mutex_unlock(&ip->mu);              /* 락 해제 */
    atomic_fetch_sub(&ip->inflight, 1);         /* 이 뒤로 ip 를 만지지 않음 */
    return n;                                   /* 개수 */
}

/* 종료: stopping 켜고 깨움 → 감시 스레드 join → 감시·대기열의 fd 모두 close
 * → 동시에 들어와 있던 호출이 다 나갈 때까지 기다림 → 자원 해제. 이후 ip 를 쓰면 안 된다 */
void idle_stop(idle_poller *ip) /* 한 번만 호출, 워커가 모두 join 된 뒤 */
{
    if (!ip)                                    /* 없는 poller */
        return;                                 /* 할 일 없음 */

    pthread_mutex_lock(&ip->mu);                /* stopping 을 mu 안에서 켜야 idle_park 와 경쟁이 없음 */
    atomic_store(&ip->stopping, 1);             /* mu 안에서 켬 → 이후 idle_park 는 대기열에 넣지 못함 */
    wake(ip);                                   /* epoll_wait/poll 에서 깨움 */
    pthread_mutex_unlock(&ip->mu);              /* 락 해제 */

    pthread_join(ip->thread, NULL);             /* 감시 스레드 종료 대기 (ready_fn 도 이제 안 불림) */

    /* 감시 중이던 연결 모두 close (epoll 은 아래에서 통째로 닫으므로 DEL 불필요) */
    idle_entry *e = ip->head;                   /* 감시 스레드가 끝났으니 락 없이 만져도 됨 */
    while (e) {                                 /* 리스트 끝까지 */
        idle_entry *next = e->next;             /* free 전에 다음 기억 */
        close(e->job.fd);                       /* 쉬던 연결 닫기 */
        free(e);                                /* 항목 반납 */
        e = next;                               /* 다음 */
    }
    ip->head = ip->tail = NULL;                 /* 리스트 비움 */

    pthread_mutex_lock(&ip->mu);                /* 대기열에만 있던 것도 close (stopping 이후로 늘지 않음) */
    e = ip->pend_head;                          /* 대기열 통째로 가져감 */
    ip->pend_head = ip->pend_tail = NULL;       /* 대기열 비움 */
    ip->npend = 0;                              /* 대기열 길이 0 */
    ip->nwatch = 0;                             /* 감시 리스트도 위에서 비웠음 */
    pthread_mutex_unlock(&ip->mu);              /* 락 해제 */
    while (e) {                                 /* 락 밖에서 close (시스템콜) */
        idle_entry *next = e->next;             /* free 전에 다음 기억 */
        close(e->job.fd);                       /* 등록 전이던 연결 닫기 */
        free(e);                                /* 항목 반납 */
        e = next;                               /* 다음 */
    }

    /* 동시에 들어와 있던 idle_park/idle_count 가 mu·pipe 를 다 쓰고 나갈 때까지 기다림 */
    while (atomic_load(&ip->inflight) > 0)      /* 아직 안에 있는 호출이 있으면 */
        sleep_ms(1);                            /* 아주 짧은 구간이라 1ms 쉬며 확인 */

#if defined(IDLE_USE_EPOLL)
    close(ip->epfd);                            /* epoll 인스턴스 (남은 등록도 함께 사라짐) */
#else
    free(ip->pfds);                             /* poll 배열 */
    free(ip->pents);                            /* 항목 포인터 배열 */
#endif
    close(ip->wake_r);                          /* wake pipe 읽는 쪽 */
    close(ip->wake_w);                          /* wake pipe 쓰는 쪽 */
    pthread_mutex_destroy(&ip->mu);             /* mutex 정리 */
    free(ip);                                   /* 구조체 반납 (이후 ip 사용 금지) */
}
