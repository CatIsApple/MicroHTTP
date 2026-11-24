#!/bin/sh
# GET 예제: 쿼리 문자열과 CGI 환경 변수를 보여 준다
printf 'Content-Type: text/plain; charset=utf-8\r\n\r\n'
echo "Hello from CGI"
echo "REQUEST_METHOD=$REQUEST_METHOD"
echo "QUERY_STRING=$QUERY_STRING"
echo "SCRIPT_NAME=$SCRIPT_NAME"
echo "PATH_INFO=$PATH_INFO"
echo "REMOTE_ADDR=$REMOTE_ADDR"
echo "SERVER_PROTOCOL=$SERVER_PROTOCOL"
