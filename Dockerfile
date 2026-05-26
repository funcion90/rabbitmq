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

# 레이어 캐시 최적화: 잘 변하지 않는 것부터 먼저 복사.
#   - third_party/ (libuv + AMQP-CPP 서브모듈) → 서브모듈 bump 시에만 변경
#   - CMakeLists.txt → 빌드 시스템 변경 시
#   - src/         → 우리 코드 (가장 자주 변경)
# src/ 만 바뀌면 third_party 컴파일 결과가 캐시에서 재사용되어 30초 이내 재빌드 가능.
COPY third_party/ /src/third_party/
COPY CMakeLists.txt /src/
COPY src/ /src/src/

# BuildKit cache mount 로 /src/build 를 빌드 간 영속화. src/ 만 바뀐 경우
# CMake 의 incremental build 가 third_party 의 객체 파일을 재사용해 약 5초 이내 완료.
# 캐시 mount 는 이미지 레이어에 포함되지 않으므로, 산출된 바이너리는 RUN 내에서
# /src 로 별도 복사하여 다음 stage 의 COPY --from 에 노출시킨다.
RUN --mount=type=cache,target=/src/build,sharing=locked \
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build --parallel --target rmq_node \
 && cp build/rmq_node /src/rmq_node

# ---------------------------------------------------------------------------
# 2) runtime
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS runtime

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        libssl3 \
        ca-certificates \
 && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/rmq_node /usr/local/bin/rmq_node

# NODE_ID 등의 환경변수는 compose 에서 주입.
CMD ["/usr/local/bin/rmq_node"]
