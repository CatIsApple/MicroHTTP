/* test.h — 외부 프레임워크 없는 아주 작은 단위 테스트 도구.
 * CHECK 계열 매크로는 실패해도 멈추지 않고 기록만 한다 → 한 번 실행으로 모든 실패를 본다.
 * 각 테스트 파일은 마지막에 TEST_REPORT() 를 return 한다 (실패 있으면 종료 코드 1). */

#ifndef TEST_H /* 인클루드 가드 */
#define TEST_H /* "이미 포함됨" 표시 */

#include <stdio.h>  /* printf, fprintf */
#include <string.h> /* strcmp (CHECK_STR) */

static int g_checks = 0; /* 실행한 검사 수 */
static int g_fails = 0;  /* 실패한 검사 수 */

/* 조건이 참인지 검사. 거짓이면 파일:줄과 조건식을 출력 */
#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                    /* 검사 수 증가 */    \
        if (!(cond)) {                                 /* 실패하면 */        \
            g_fails++;                                 /* 실패 수 증가 */    \
            fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0) /* do-while(0): 매크로를 if 문 안에서도 문장 하나처럼 쓰기 위한 관용구 */

/* 두 정수가 같은지 검사. 실패 시 실제 값과 기대 값을 함께 출력 */
#define CHECK_INT(actual, expected)                                          \
    do {                                                                     \
        long long a_ = (long long)(actual), e_ = (long long)(expected);     \
        g_checks++;                                                          \
        if (a_ != e_) {                                                      \
            g_fails++;                                                       \
            fprintf(stderr, "%s:%d: FAIL: %s == %lld (expected %lld)\n",     \
                    __FILE__, __LINE__, #actual, a_, e_);                    \
        }                                                                    \
    } while (0)

/* 두 문자열이 같은지 검사 (NULL 도 안전하게 비교) */
#define CHECK_STR(actual, expected)                                                  \
    do {                                                                             \
        const char *a_ = (actual), *e_ = (expected);                                 \
        g_checks++;                                                                  \
        if (!((a_ == NULL && e_ == NULL) ||                                          \
              (a_ != NULL && e_ != NULL && strcmp(a_, e_) == 0))) {                  \
            g_fails++;                                                               \
            fprintf(stderr, "%s:%d: FAIL: %s == \"%s\" (expected \"%s\")\n",         \
                    __FILE__, __LINE__, #actual, a_ ? a_ : "(null)", e_ ? e_ : "(null)"); \
        }                                                                            \
    } while (0)

/* 결과 요약 출력 후 종료 코드 반환 (main 의 마지막 줄에서 return TEST_REPORT();) */
#define TEST_REPORT() \
    (printf("%s: %d checks, %d failed\n", __FILE__, g_checks, g_fails), g_fails ? 1 : 0)

#endif /* TEST_H */
