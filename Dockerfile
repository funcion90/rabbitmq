# syntax=docker/dockerfile:1.6
#
# 멀티 스테이지 빌드:
#   1) builder   — 빌드 도구 + OpenSSL dev 헤더 설치, cmake configure/build 수행
#   2) runtime   — 산출된 바이너리만 들고 가는 슬림 이미지
#
# 호스트가 Windows/Mac이어도 컨테이너 내부는 항상 Linux이므로
# AMQP-CPP linux_tcp 모듈이 정상 빌드된다.

# ---------------------------------------------------------------------------
# 1) builder
#
# Ubuntu 24.04 = GCC 13.x = libstdc++ 13 = std::format 사용 가능.
# uvx::log 가 std::format 을 광범위하게 사용하므로 22.04(GCC 11) 는 부적합.
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS builder

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        libssl-dev \
        ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

# 서브모듈이 이미 체크아웃되어 함께 COPY 된 상태이므로 별도 init 불필요.
RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build --parallel --target rmq_node

# ---------------------------------------------------------------------------
# 2) runtime
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS runtime

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        libssl3 \
        ca-certificates \
 && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/rmq_node /usr/local/bin/rmq_node

# NODE_ID 등의 환경변수는 compose 에서 주입.
CMD ["/usr/local/bin/rmq_node"]
