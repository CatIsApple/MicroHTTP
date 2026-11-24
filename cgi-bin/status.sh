#!/bin/sh
# Status 헤더 예제: 스크립트가 직접 상태 코드를 정한다
printf 'Status: 404 Not Found\r\nContent-Type: text/plain\r\n\r\nno such item\n'
