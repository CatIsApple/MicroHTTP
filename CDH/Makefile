# Makefile — microhttp 빌드 규칙.
# 사용법: make (빌드) / make asan (메모리 검사 빌드) / make clean (산출물 삭제)
# 주의: 변수 줄 끝에 주석을 붙이면 값 뒤에 공백이 남을 수 있어서, 주석은 모두 별도 줄에 둔다.

# 컴파일러. ?= 는 "환경변수로 따로 지정 안 했을 때만" 기본값 gcc 를 쓴다는 뜻
CC      ?= gcc
# 컴파일 옵션: C11 표준, 경고 최대(-Wall -Wextra -Wpedantic), 최적화 -O2, 디버그 정보 -g
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g
# 전처리 옵션: include/ 폴더를 헤더 검색 경로에 추가 (#include "tcp_server.h" 를 찾게 함)
# CFLAGS 와 분리한 이유: make asan 이 CFLAGS 를 통째로 바꿔도 이 경로는 유지되도록
CPPFLAGS += -Iinclude

# 라이브러리 소스: main 을 뺀 모듈들 (서버 실행 파일과 단위 테스트가 함께 씀)
LIB_SRCS := src/tcp_server.c src/http_parser.c src/http_body.c src/http_response.c
# 서버 소스 목록 = main + 라이브러리
SRCS := src/main.c $(LIB_SRCS)
# 오브젝트 파일 목록: src/xxx.c → build/xxx.o 로 이름만 바꿔서 만듦
OBJS := $(SRCS:src/%.c=build/%.o)
# 최종 실행 파일 이름
BIN  := microhttp

# 실제 파일 이름이 아닌 "명령" 타깃이라고 표시 (같은 이름의 파일이 있어도 항상 실행)
.PHONY: all clean asan test

# 기본 타깃: 그냥 make 하면 실행 파일을 만든다
all: $(BIN)

# 링크: 모든 .o 를 합쳐 실행 파일 생성. $@ = 타깃 이름(microhttp), $^ = 모든 의존 파일(.o 들)
$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# 컴파일: .c 하나 → .o 하나. include/ 의 헤더가 바뀌어도 다시 컴파일되도록 의존성에 헤더 포함.
# "| build" = build 폴더가 있어야 함(order-only: 폴더 시각이 바뀌었다고 다시 컴파일하지는 않음). $< = 첫 번째 의존 파일(.c)
build/%.o: src/%.c $(wildcard include/*.h) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# build 폴더 만들기 (-p: 이미 있어도 에러 없음)
build:
	mkdir -p build

# 메모리 검사 빌드: 먼저 clean 한 뒤, AddressSanitizer(메모리 오류) + UndefinedBehaviorSanitizer(정의되지 않은 동작)를 켜고 다시 make.
# -O1 + -fno-omit-frame-pointer 는 오류 발생 시 호출 스택을 정확히 보여 주기 위함
asan: clean
	$(MAKE) CFLAGS="-std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,undefined"

# ---- 단위 테스트 ----
# tests/test_*.c 하나가 실행 파일 하나 (build/tests/test_xxx).
# 항상 sanitizer 를 켜서 빌드 → 메모리 오류·정의되지 않은 동작도 테스트 실패로 잡는다.
TEST_SRCS := $(wildcard tests/test_*.c)
TEST_BINS := $(TEST_SRCS:tests/%.c=build/tests/%)
TEST_CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer

# make test: 모든 테스트를 빌드한 뒤 차례로 실행. 하나라도 실패하면 make 도 실패(종료 코드 != 0)
test: $(TEST_BINS)
	@for t in $(TEST_BINS); do ./$$t || exit 1; done
	@echo "all tests passed"

# 테스트 실행 파일 하나 = 테스트 .c + 라이브러리 소스 전부 (main.c 제외)
build/tests/%: tests/%.c $(LIB_SRCS) $(wildcard include/*.h) tests/test.h | build/tests
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ $< $(LIB_SRCS)

build/tests:
	mkdir -p build/tests

# 빌드 산출물(build 폴더, 실행 파일) 삭제
clean:
	rm -rf build $(BIN)
