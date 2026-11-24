# MicroHTTP 빌드
#   make            서버 빌드 (./microhttp)
#   make test       단위 테스트
#   make e2e        서버를 띄워 curl/nc 로 확인
#   make asan       AddressSanitizer + UBSan 빌드
#   make asan-test  sanitizer 를 켜고 단위 테스트
#   make clean

CC       ?= cc
CFLAGS   ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g
CPPFLAGS += -Iinclude -MMD -MP
SAN      := -fsanitize=address,undefined -fno-omit-frame-pointer

BIN      := microhttp
LIB_SRCS := src/tcp.c src/http_request.c src/http_response.c src/conn.c src/server.c
LIB_OBJS := $(LIB_SRCS:src/%.c=build/%.o)
TESTS    := build/test_tcp build/test_http_request build/test_http_response

.PHONY: all test e2e asan asan-test clean
all: $(BIN)

$(BIN): build/main.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

build/%.o: src/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

build/test_%.o: tests/test_%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

build/test_%: build/test_%.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

build:
	mkdir -p build

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

e2e: $(BIN)
	./tests/e2e.sh ./$(BIN)

asan: clean
	$(MAKE) CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -O1 -g $(SAN)" LDFLAGS="$(SAN)"

asan-test: clean
	$(MAKE) test CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -O1 -g $(SAN)" LDFLAGS="$(SAN)"

clean:
	rm -rf build $(BIN)

-include $(wildcard build/*.d)
