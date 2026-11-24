#!/bin/sh
# 시간 초과 예제: 서버의 CGI 제한(5초)보다 오래 걸려 504 가 나와야 한다
sleep 30
printf 'Content-Type: text/plain\r\n\r\ntoo late\n'
