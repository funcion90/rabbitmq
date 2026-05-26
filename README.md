# rabbitmq-sample

libuv 이벤트 루프 + AMQP-CPP 로 RabbitMQ 와 통신하는 학습용 ping-pong 노드 샘플.

동일한 `rmq_node` 바이너리 인스턴스를 둘 띄우면, 각각 `fanout` exchange 에 발행 + 자기 큐를 구독하여 서로의 메시지를 함께 본다 (자기 자신 메시지는 `[SELF]`, 상대 메시지는 `[ RX ]` 마킹).

Linux/Docker 환경에서 빌드·실행되며, 호스트가 Windows / macOS 여도 Docker Compose 만 있으면 전체 토폴로지(broker + 2 node)가 그대로 동작한다.

---

## 빠른 시작 (Docker Compose, 권장)

compose 가 두 개로 분리되어 있다:

| 파일 | 책임 |
|------|------|
| `docker/broker/docker-compose.yml` | RabbitMQ 브로커 단독 — named network `rmq_net` 생성, 다른 도구 테스트에 재활용 가능 |
| `docker/sample/docker-compose.yml` | `rmq_node` 두 인스턴스(node_a, node_b) — broker 의 `rmq_net` 에 join |

```bash
git clone --recursive https://github.com/funcion90/rabbitmq.git
cd rabbitmq

# 1) broker 띄우기 (백그라운드)
docker compose -f docker/broker/docker-compose.yml up -d

# 2) ping-pong 샘플 실행 (node_a 가 끝나면 정리)
docker compose -f docker/sample/docker-compose.yml up --build \
    --abort-on-container-exit --exit-code-from node_a

# 3) broker 도 내리기
docker compose -f docker/broker/docker-compose.yml down
```

- 콘솔에 `[A] PUB [A] msg #1`, `[B] RX from=A : [A] msg #1` 같은 ping-pong 로그가 흘러간다.
- RabbitMQ 관리 콘솔: <http://localhost:15672>  (계정 `guest` / `guest`)
- broker 만 띄워두고 sample 을 여러 번 재실행해도 같은 exchange/큐를 그대로 쓴다.

> **`--recursive`** 를 빠뜨리면 `third_party/libuv`, `third_party/AMQP-CPP` 가 비어 있다. 그럴 땐 다음으로 복구:
> ```bash
> git submodule update --init --recursive
> ```

---

## 빠른 시작 (Linux / WSL 네이티브)

**Ubuntu 24.04 이상** 권장 (GCC 13.x — `std::format` 사용 가능). 22.04 는 GCC 11 이라 `<format>` 헤더가 없어 컴파일 실패:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git libssl-dev

git clone --recursive https://github.com/funcion90/rabbitmq.git
cd rabbitmq

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel --target rmq_node

# 별도 터미널에서 RabbitMQ 브로커 띄우기
docker run -d --rm --name rmq -p 5672:5672 -p 15672:15672 rabbitmq:3-management

# 두 터미널에서 node 두 인스턴스 실행
NODE_ID=A ./build/rmq_node
NODE_ID=B ./build/rmq_node
```

---

## 네이티브 Windows 는 미지원

AMQP-CPP 의 `linux_tcp` 모듈이 POSIX 전용(`sys/socket`, `poll.h`, OpenSSL `dlopen`)이라 MSVC 로 직접 빌드되지 않는다. Windows 사용자는 다음 중 하나를 선택한다.

| 옵션 | 동작 |
|------|------|
| Docker Desktop + `docker compose up` | 컨테이너 안이 Linux 라 그대로 동작 (권장) |
| WSL2 + Ubuntu | 위의 "Linux 네이티브" 절차와 동일 |

CMake 가 Windows 빌드를 시도하면 `FATAL_ERROR` 로 즉시 중단되어 잘못된 시도임을 알려준다.

---

## 구조

```
rabbitmq/
├── CMakeLists.txt
├── Dockerfile                            # 이미지 빌드 정의 (ubuntu:24.04 멀티스테이지)
├── docker/
│   ├── broker/
│   │   └── docker-compose.yml            # broker 단독 — named network 'rmq_net' 생성
│   └── sample/
│       └── docker-compose.yml            # publisher + consumer — 'rmq_net' external join
├── third_party/
│   ├── libuv/        # 서브모듈: github.com/funcion90/libuv
│   │                 #   FetchContent 로 libuv v1.52.1 본체 자동 다운로드
│   │                 #   uvx::log / uvx::check / uv_a 타겟 제공
│   └── AMQP-CPP/     # 서브모듈: AMQP-CPP v4.3.27
│                     #   amqpcpp 정적 라이브러리 + AMQP::LibUvHandler 헤더
└── src/
    └── node/main.cpp        # pub + sub 통합. fanout exchange + 노드별 큐.
                             #   SIGINT/SIGTERM 받으면 graceful shutdown.
```

### 의존성 다이어그램

```
 rmq_node
    │
    ├── uvx_common  ──► uv_a (libuv 정적 라이브러리, v1.52.1)
    │                   + include path: third_party/libuv/src
    │                   + 헤더: uvx::log, uvx::check, uvx::format_ip4
    │
    └── amqpcpp     ──► AMQP-CPP linux_tcp 모듈 (POSIX TCP + OpenSSL dlopen)
                        + 헤더: <amqpcpp.h>, <amqpcpp/libuv.h>
```

---

## 환경 변수

| 변수 | 기본값 | 설명 |
|------|--------|------|
| `NODE_ID` | `node` | 인스턴스 식별자. 큐 이름 `sample.node.<ID>` 에 들어가고 메시지 sender 헤더로도 사용 |
| `RABBITMQ_URL` | `amqp://guest:guest@localhost/` | 브로커 접속 URL |
| `RABBITMQ_EXCHANGE` | `sample.fanout` | 사용할 fanout exchange 이름 |
| `PUBLISH_COUNT` | `10` | 인스턴스가 발행할 메시지 개수 |
| `PUBLISH_INTERVAL_MS` | `500` | 발행 간격 |
| `LINGER_MS` | `3000` | 발행 완료 후 in-flight 메시지 수신 여유 시간 |

---

## 서브모듈 업데이트

`funcion90/libuv` 가 새 commit 을 받았다거나 AMQP-CPP 의 새 태그를 따라가고 싶을 때:

```bash
# libuv 최신 main 으로
cd third_party/libuv && git pull origin main && cd ../..
git add third_party/libuv
git commit -m "chore: bump funcion90/libuv submodule"

# AMQP-CPP 특정 태그로
cd third_party/AMQP-CPP && git fetch --tags && git checkout v4.3.28 && cd ../..
git add third_party/AMQP-CPP
git commit -m "chore: bump AMQP-CPP to v4.3.28"
```

---

## 라이선스

- 본 샘플 코드: 별도 표기 없으면 MIT.
- `third_party/libuv` (funcion90/libuv) 및 내부에서 받아오는 upstream libuv: MIT-like.
- `third_party/AMQP-CPP`: Apache 2.0.
