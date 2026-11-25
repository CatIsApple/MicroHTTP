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
PYTHON  ?= python3

SOURCES = src/http.c src/net.c src/static_file.c src/proxy.c
HEADERS = $(wildcard src/*.h)
UNIT_TESTS = build/test_http build/test_mime build/test_static build/test_net build/test_proxy

.PHONY: all test unit functional asan clean

all: server

server: src/server.c $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) src/server.c $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

build:
	mkdir -p build

# test_net.c는 net.c를 직접 #include 해서 send()를 가짜로 바꾸므로 net.c를 따로 링크하지 않습니다.
build/test_net: tests/unit/test_net.c src/net.c src/http.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_net.c src/http.c $(LDFLAGS) $(LDLIBS) -o $@

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

clean:
	rm -rf server build *.dSYM
