# Linux 검증용 이미지: gcc(빌드) + valgrind(메모리) + python3(기능 테스트) + curl
#   docker build -t microhttp-verify tests/linux
#   docker run --rm -v "$PWD":/src:ro microhttp-verify sh /src/tests/linux/verify.sh
FROM ubuntu:24.04
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential valgrind python3 curl \
 && rm -rf /var/lib/apt/lists/* \
 && useradd -m tester
