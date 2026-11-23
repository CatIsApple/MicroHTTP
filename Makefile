CC = cc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror

.PHONY: all run test clean
all: server

server: server.c
	$(CC) $(CFLAGS) server.c $(LDFLAGS) -o $@

run: server
	./server

tests/test_send: tests/test_send.c server.c
	$(CC) $(CFLAGS) tests/test_send.c $(LDFLAGS) -o $@

test: server tests/test_send
	./tests/test_send
	python3 tests/test_server.py

clean:
	rm -f server tests/test_send
