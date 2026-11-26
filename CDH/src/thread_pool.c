/* thread_pool.c — 고정 크기 워커 스레드 풀 + 상한 있는 작업 큐 (선언은 include/thread_pool.h).
 *
 *   accept 스레드 / idle poller ──tp_submit──▶ [고리 버퍼 큐] ──▶ 워커 N개 ──fn(job, ctx)
 *
 * - 큐는 conn_job 고리 버퍼 하나. mutex 로 보호하고, 비었을 때 잠드는 워커는 cond(not_empty) 로 깨운다.
 * - tp_submit 은 절대 기다리지 않는다: 가득이면 EAGAIN, 종료 중이면 ECANCELED 로 즉시 -1.
 * - 워커는 job 을 지역 변수로 복사해 락 밖에서 fn 을 부른다 (fn 이 오래 걸려도 큐를 막지 않도록).
 * - 종료 알림 pipe: tp_shutdown 이 쓰는 쪽에 1바이트를 쓰면, 읽는 쪽(tp_wake_fd)은 아무도 읽지 않으니
 *   영원히 "읽기 가능" 으로 남는다 → 다른 스레드가 poll 에 넣어 두면 종료 때 즉시 깨어난다.
 * - 수명: tp_shutdown 은 스레드만 정리하고 메모리는 남긴다 (이후 다른 스레드의 tp_submit 이 안전하게 실패하도록).
 *   메모리·pipe 는 tp_destroy 에서만 해제한다. 종료 순서: tp_shutdown → idle_stop → tp_destroy. */

#define _POSIX_C_SOURCE 200809L /* POSIX 2008 (pthread_sigmask, pthread_attr_* 등)를 헤더에서 보이게 함. include 보다 먼저 와야 함 */

#include "thread_pool.h" /* 이 파일이 구현할 함수들의 선언 (선언과 구현이 일치하는지 컴파일러가 검사) */

#include <errno.h>     /* errno, EINVAL, EAGAIN, ECANCELED, EINTR */
#include <fcntl.h>     /* fcntl, F_SETFD, FD_CLOEXEC (pipe 의 두 fd 를 exec 때 닫히게) */
#include <limits.h>    /* PTHREAD_STACK_MIN (스레드 스택 최소 크기) */
#include <pthread.h>   /* pthread_*: 스레드, mutex, cond, attr */
#include <signal.h>    /* sigset_t, sigemptyset, sigaddset, pthread_sigmask, SIGINT, SIGTERM */
#include <stdatomic.h> /* atomic_int (stopping 을 락 없이 읽기 위해) */
#include <stdint.h>    /* SIZE_MAX (qcap 곱셈 오버플로 검사) */
#include <stdlib.h>    /* calloc, free */
#include <unistd.h>    /* pipe, write, close, sysconf */

#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 16384 /* 이 값을 정의하지 않는 환경 대비 보수적 기본값 */
#endif

#define TP_DEFAULT_STACK (256u * 1024u) /* stack_size 가 0 일 때 쓰는 워커 스택 크기 (256KB) */

struct thread_pool {
    pthread_t *threads;       /* 워커 스레드 핸들 배열 */
    int nthreads;             /* 실제로 만들어진 워커 수 (join 대상) */
    conn_job *queue;          /* 고리 버퍼 본체 (qcap 칸) */
    size_t qcap;              /* 큐 용량 (생성 후 불변 → 락 없이 읽어도 됨) */
    size_t head;              /* 다음에 꺼낼 칸 */
    size_t count;             /* 지금 들어 있는 작업 수 */
    pthread_mutex_t mu;       /* head/count/queue 와 cond 를 보호 */
    pthread_cond_t not_empty; /* "큐에 작업이 생겼거나 종료 중" 신호 */
    int mu_ok;                /* mutex 를 초기화했는지 (정리 코드가 destroy 해도 되는지 판단) */
    int cv_ok;                /* cond 를 초기화했는지 */
    atomic_int stopping;      /* tp_shutdown 시작 여부 (락 없이 읽음) */
    int wake[2];              /* 종료 알림 pipe: [0] 읽는 쪽, [1] 쓰는 쪽 */
    tp_job_fn fn;             /* 워커가 작업마다 부르는 함수 */
    void *ctx;                /* fn 에 그대로 넘기는 사용자 포인터 */
};

/* 워커 스레드 본체: 큐에서 하나씩 꺼내 fn 을 부르고, 종료 신호가 오면 나간다 */
static void *tp_worker(void *arg)
{
    thread_pool *tp = arg; /* pthread_create 가 넘겨준 풀 */

    for (;;) { /* 종료할 때까지 반복 */
        pthread_mutex_lock(&tp->mu); /* 큐를 만지기 전에 락 */
        while (tp->count == 0 && !atomic_load(&tp->stopping)) { /* 할 일도 없고 종료도 아니면 */
            pthread_cond_wait(&tp->not_empty, &tp->mu);          /* 락을 풀고 잠듦 (깨어나면 락을 다시 잡음, 가짜 깨어남 대비 while) */
        }
        if (atomic_load(&tp->stopping)) {  /* 종료 중이면 남은 큐 작업은 처리하지 않음 */
            pthread_mutex_unlock(&tp->mu); /* 락 반납 후 */
            break;                         /* 루프 탈출 → 스레드 종료 (남은 fd 는 tp_shutdown 이 close) */
        }

        conn_job job = tp->queue[tp->head];       /* 맨 앞 작업을 지역 변수로 복사 (락 밖에서 안전하게 쓰려고) */
        tp->head = (tp->head + 1) % tp->qcap;     /* 다음 칸으로 (끝이면 처음으로 되감김) */
        tp->count--;                              /* 큐 항목 하나 줄어듦 */
        pthread_mutex_unlock(&tp->mu);            /* fn 은 오래 걸릴 수 있으니 락 밖에서 호출 */

        tp->fn(&job, tp->ctx); /* 작업 처리: job.fd 의 소유권은 이제 fn 에게 있음 */
    }
    return NULL; /* 반환값은 쓰지 않음 */
}

/* 워커 전체를 깨워 종료시키고 join. 호출 전에 stopping 이 이미 1 이어야 한다 */
static void tp_stop_workers(thread_pool *tp)
{
    pthread_mutex_lock(&tp->mu);          /* 락을 잡았다 놓아야 "막 잠들려는 워커" 가 신호를 놓치지 않음 */
    pthread_cond_broadcast(&tp->not_empty); /* 잠든 워커 전부 깨움 (깨어나면 stopping 을 보고 나감) */
    pthread_mutex_unlock(&tp->mu);        /* 락 반납 */
    for (int i = 0; i < tp->nthreads; i++) { /* 만들어진 워커만 */
        pthread_join(tp->threads[i], NULL);  /* fn 이 끝나고 스레드가 나갈 때까지 기다림 */
    }
    tp->nthreads = 0; /* 이미 join 했으니 다시 join 하지 않도록 */
}

/* 메모리·pipe·동기화 객체를 해제 (부분적으로만 만들어진 풀도 안전하게 처리) */
static void tp_free(thread_pool *tp)
{
    if (tp->wake[0] >= 0) close(tp->wake[0]); /* 읽는 쪽 pipe 닫기 */
    if (tp->wake[1] >= 0) close(tp->wake[1]); /* 쓰는 쪽 pipe 닫기 */
    if (tp->cv_ok) pthread_cond_destroy(&tp->not_empty); /* cond 정리 */
    if (tp->mu_ok) pthread_mutex_destroy(&tp->mu);       /* mutex 정리 */
    free(tp->queue);   /* 고리 버퍼 */
    free(tp->threads); /* 스레드 핸들 배열 */
    free(tp);          /* 풀 자체 */
}

thread_pool *tp_create(int nthreads, size_t qcap, size_t stack_size, tp_job_fn fn, void *ctx)
{
    if (nthreads < 1 || qcap < 1 || fn == NULL) { /* 잘못된 인자 */
        errno = EINVAL;                           /* 호출자 실수 */
        return NULL;
    }
    if (qcap > SIZE_MAX / sizeof(conn_job)) { /* qcap * sizeof 가 넘치면 malloc 크기가 틀어짐 */
        errno = ENOMEM;
        return NULL;
    }

    thread_pool *tp = calloc(1, sizeof(*tp)); /* 0 으로 채워 시작 → 정리 코드가 "만든 것만" 해제 가능 */
    if (tp == NULL) return NULL;              /* calloc 이 errno 를 ENOMEM 으로 설정 */
    tp->wake[0] = tp->wake[1] = -1;           /* pipe 아직 없음 표시 */
    tp->qcap = qcap;                          /* 큐 용량 */
    tp->fn = fn;                              /* 작업 함수 */
    tp->ctx = ctx;                            /* 사용자 포인터 */
    atomic_init(&tp->stopping, 0);            /* 종료 플래그 초기화 */

    int err = 0; /* 실패 원인 (pthread 함수들은 errno 가 아니라 반환값으로 알려 줌) */

    tp->threads = calloc((size_t)nthreads, sizeof(pthread_t));    /* 스레드 핸들 배열 */
    tp->queue = calloc(qcap, sizeof(conn_job));                   /* 고리 버퍼 */
    if (tp->threads == NULL || tp->queue == NULL) {
        err = ENOMEM;
        goto fail;
    }

    if ((err = pthread_mutex_init(&tp->mu, NULL)) != 0) goto fail; /* 큐 보호용 mutex */
    tp->mu_ok = 1;
    if ((err = pthread_cond_init(&tp->not_empty, NULL)) != 0) goto fail; /* "작업 생김" 신호용 cond */
    tp->cv_ok = 1;

    if (pipe(tp->wake) < 0) { /* 종료 알림 pipe */
        err = errno;
        tp->wake[0] = tp->wake[1] = -1; /* 실패 시 값이 불확정일 수 있으니 닫지 않도록 되돌림 */
        goto fail;
    }
    for (int i = 0; i < 2; i++) { /* 양쪽 모두 exec 시 자동으로 닫히게 (자식 프로세스에 새지 않도록) */
        if (fcntl(tp->wake[i], F_SETFD, FD_CLOEXEC) < 0) {
            err = errno;
            goto fail;
        }
    }

    pthread_attr_t attr; /* 워커 스레드 속성 (스택 크기) */
    if ((err = pthread_attr_init(&attr)) != 0) goto fail;
    size_t ss = stack_size ? stack_size : TP_DEFAULT_STACK; /* 0 이면 기본값 */
    if (ss < (size_t)PTHREAD_STACK_MIN) ss = (size_t)PTHREAD_STACK_MIN; /* 최소 크기 미만이면 올림 */
    long page = sysconf(_SC_PAGESIZE);                      /* macOS 는 스택 크기가 페이지 배수여야 함 */
    if (page > 0) ss = (ss + (size_t)page - 1) / (size_t)page * (size_t)page; /* 페이지 배수로 올림 */
    if ((err = pthread_attr_setstacksize(&attr, ss)) != 0) {
        pthread_attr_destroy(&attr);
        goto fail;
    }

    sigset_t block, old; /* block: 막을 시그널, old: 원래 마스크 */
    sigemptyset(&block);
    sigaddset(&block, SIGINT);  /* Ctrl-C */
    sigaddset(&block, SIGTERM); /* 종료 요청 */
    pthread_sigmask(SIG_BLOCK, &block, &old); /* 생성하는 동안 막음 → 새 스레드가 막힌 마스크를 물려받음 (종료 시그널은 메인만 받음) */

    for (int i = 0; i < nthreads; i++) { /* 워커 N개 생성 */
        err = pthread_create(&tp->threads[i], &attr, tp_worker, tp);
        if (err != 0) break;  /* 중간 실패: 이미 만든 것만 정리 */
        tp->nthreads = i + 1; /* join 대상 수 갱신 (성공한 것만 셈) */
    }

    pthread_sigmask(SIG_SETMASK, &old, NULL); /* 호출 스레드의 마스크 원상 복구 */
    pthread_attr_destroy(&attr);              /* 속성 객체는 더 필요 없음 */

    if (err != 0) {                  /* 일부만 만들어졌다면 */
        atomic_store(&tp->stopping, 1); /* 종료 플래그 → */
        tp_stop_workers(tp);            /* 만든 워커를 깨워 join */
        goto fail;
    }
    return tp; /* 성공 */

fail:
    tp_free(tp);  /* 만들어진 자원 모두 해제 (close/free 가 errno 를 바꿀 수 있어서) */
    errno = err;  /* 원래 실패 원인을 복원 */
    return NULL;
}

int tp_submit(thread_pool *tp, const conn_job *job)
{
    pthread_mutex_lock(&tp->mu); /* stopping 확인과 push 를 한 락 안에서 → shutdown 의 큐 비우기와 경쟁하지 않음 */
    if (atomic_load(&tp->stopping)) { /* 종료 중이면 받지 않음 (fd 는 호출자 몫) */
        pthread_mutex_unlock(&tp->mu);
        errno = ECANCELED;
        return -1;
    }
    if (tp->count == tp->qcap) { /* 큐 가득: 기다리지 않고 바로 실패 (accept 스레드가 막히면 안 됨) */
        pthread_mutex_unlock(&tp->mu);
        errno = EAGAIN;
        return -1;
    }
    tp->queue[(tp->head + tp->count) % tp->qcap] = *job; /* 맨 뒤 칸에 복사 (head+count 가 다음 빈 칸) */
    tp->count++;                                         /* 항목 하나 늘어남 */
    pthread_cond_signal(&tp->not_empty);                 /* 잠든 워커 하나를 깨움 */
    pthread_mutex_unlock(&tp->mu);
    return 0; /* 성공: fd 소유권이 풀로 넘어감 */
}

size_t tp_queued(thread_pool *tp)
{
    pthread_mutex_lock(&tp->mu); /* count 는 락으로 보호되는 값 → 락 안에서 읽어 경쟁을 피함 */
    size_t n = tp->count;
    pthread_mutex_unlock(&tp->mu);
    return n; /* 반환 직후에는 이미 변했을 수 있는 근사값 */
}

size_t tp_capacity(thread_pool *tp)
{
    return tp->qcap; /* 생성 후 바뀌지 않는 값이라 락 불필요 */
}

int tp_stopping(thread_pool *tp)
{
    return atomic_load(&tp->stopping) ? 1 : 0; /* 락 없이 읽기 (어느 스레드에서든 안전) */
}

int tp_wake_fd(thread_pool *tp)
{
    return tp->wake[0]; /* 종료 알림 pipe 의 읽는 쪽 (tp_destroy 까지 유효) */
}

void tp_shutdown(thread_pool *tp)
{
    if (atomic_exchange(&tp->stopping, 1)) return; /* 이미 종료 시작됨: 두 번째 호출은 아무 일도 안 함 (join 중복 방지) */

    char byte = 1; /* 내용은 무관, "무언가 있다" 만 중요 */
    ssize_t w;
    do {
        w = write(tp->wake[1], &byte, 1); /* 읽는 쪽이 영원히 readable 이 됨 (아무도 읽지 않음) */
    } while (w < 0 && errno == EINTR);    /* 시그널로 끊기면 재시도 */

    tp_stop_workers(tp); /* 잠든 워커 모두 깨우고, 처리 중인 fn 이 끝나길 기다려 join */

    pthread_mutex_lock(&tp->mu); /* 워커는 모두 끝났지만, 이후 tp_submit 과의 경쟁은 락으로 막음 */
    while (tp->count > 0) {      /* 처리되지 못한 작업이 남았으면 */
        int fd = tp->queue[tp->head].fd;
        if (fd >= 0) close(fd);                /* 소유권이 풀에 있으므로 여기서 닫음 */
        tp->head = (tp->head + 1) % tp->qcap;  /* 다음 항목 */
        tp->count--;
    }
    pthread_mutex_unlock(&tp->mu);
    /* 메모리는 해제하지 않음 → tp_destroy 에서 */
}

void tp_destroy(thread_pool *tp)
{
    if (tp == NULL) return; /* NULL 은 무시 */
    tp_free(tp);            /* pipe 두 fd, 동기화 객체, 메모리 해제 */
}
