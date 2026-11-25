# MicroHTTP — 학습용 C 웹 서버
#
#   make            서버 빌드 (./server)
#   make test       단위 테스트 + 기능 테스트
#   make asan       AddressSanitizer/UBSan으로 빌드 후 전체 테스트
#   make clean      빌드 결과 삭제

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror
# _XOPEN_SOURCE=700 = POSIX.1-2008 + XSI 확장. Linux glibc는 _POSIX_C_SOURCE만으로는 realpath()를 선언하지 않습니다.
CPPFLAGS += -D_XOPEN_SOURCE=700 -Isrc
LDLIBS  += -lpthread
# 비밀번호 해싱(argon2id)과 CSPRNG는 검증된 라이브러리 libsodium을 씁니다(직접 구현 금지).
# macOS Homebrew는 기본 경로 밖에 설치되므로 위치를 찾아 붙입니다. LDLIBS에 넣어야
# make asan/tsan처럼 LDFLAGS를 덮어쓰는 빌드에서도 링크됩니다.
SODIUM_PREFIX := $(shell brew --prefix libsodium 2>/dev/null)
ifneq ($(SODIUM_PREFIX),)
CPPFLAGS += -I$(SODIUM_PREFIX)/include
LDLIBS   += -L$(SODIUM_PREFIX)/lib
endif
LDLIBS  += -lsodium
PYTHON  ?= python3

SOURCES = src/http.c src/net.c src/static_file.c src/proxy.c src/session.c src/auth.c
HEADERS = $(wildcard src/*.h)
UNIT_TESTS = build/test_http build/test_mime build/test_static build/test_net build/test_proxy build/test_session

.PHONY: all test unit functional asan tsan clean

all: server

server: src/server.c $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) src/server.c $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

build:
	mkdir -p build

# test_net.c는 net.c를 직접 #include 해서 send()를 가짜로 바꾸므로 net.c를 따로 링크하지 않습니다.
build/test_net: tests/unit/test_net.c src/net.c src/http.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_net.c src/http.c $(LDFLAGS) $(LDLIBS) -o $@

# 비밀번호 해시 만들기: echo -n 'password' | build/hashpw
build/hashpw: tools/hashpw.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

# STEP 1 실험용: 사용자를 연결(fd) 번호로 식별하는 잘못된 설계
build/server_fd_demo: src/server.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) -DAUTH_FD_DEMO $(CFLAGS) src/server.c $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

build/test_%: tests/unit/test_%.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

unit: $(UNIT_TESTS)
	@for test in $(UNIT_TESTS); do ./$$test || exit 1; done

functional: server
	$(PYTHON) tests/functional/test_server.py

test: unit functional

asan:
	$(MAKE) clean
	$(MAKE) test CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined'

# ThreadSanitizer: 스레드 사이의 데이터 레이스 검사 (ASan과 동시에 켤 수 없어 따로 빌드)
tsan:
	$(MAKE) clean
	$(MAKE) test CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=thread -fno-omit-frame-pointer' LDFLAGS='-fsanitize=thread'

clean:
	rm -rf server build *.dSYM
