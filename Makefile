CC = cc
PYTHON = python3
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror

.PHONY: all run test test-unit clean
all: server

football_page.inc: web/match.html tools/embed_page.py
	$(PYTHON) tools/embed_page.py

server: server.c http.c http.h football.c football.h chat.c chat.h football_page.inc
	$(CC) $(CFLAGS) server.c http.c football.c chat.c $(LDFLAGS) -o $@

run: server
	./server

tests/test_send: tests/test_send.c server.c http.c http.h football.c football.h chat.c chat.h football_page.inc
	$(CC) $(CFLAGS) tests/test_send.c http.c football.c chat.c $(LDFLAGS) -o $@

tests/test_http: tests/test_http.c http.c http.h football.c football.h chat.c chat.h football_page.inc
	$(CC) $(CFLAGS) tests/test_http.c http.c football.c chat.c $(LDFLAGS) -o $@

tests/test_chat: tests/test_chat.c chat.c chat.h
	$(CC) $(CFLAGS) tests/test_chat.c chat.c $(LDFLAGS) -o $@

test-unit: tests/test_send tests/test_http tests/test_chat
	./tests/test_send
	./tests/test_http
	./tests/test_chat

test: server test-unit
	$(PYTHON) tests/test_server.py

clean:
	rm -f server tests/test_send tests/test_http tests/test_chat football_page.inc
