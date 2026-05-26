---
name: simulate-docker
description: 본 rabbitmq 프로젝트의 RabbitMQ broker + 2-node ping-pong 토폴로지를 Docker Compose 로 띄워 end-to-end 동작을 검증한다. 잔재 정리 → broker compose 기동 → check_port_connectivity healthcheck 대기 → sample compose 실행(--abort-on-container-exit --exit-code-from node_a) → ping-pong 로그 확인 → broker teardown 까지 한 번에 수행한다. 사용자가 "도커로 시뮬레이션", "ping-pong 돌려봐", "broker + 노드 띄워서 확인", "메시지 흐름 검증", "compose 실행", "simulate-docker", "rabbitmq 샘플 시뮬레이션", 또는 비슷한 의도로 이 프로젝트의 도커 토폴로지가 동작하는지 확인하고 싶을 때 이 스킬을 사용한다.
---

# simulate-docker

이 프로젝트의 `docker/broker/docker-compose.yml` + `docker/sample/docker-compose.yml` 두 토폴로지를 순서대로 띄워 end-to-end 검증한다. 손으로 6~7개 명령을 치고 healthcheck 폴링을 기다리고 정리까지 하는 과정을 한 호출로 묶는다.

## 식별자 맵 (헷갈리기 쉬움)

| 종류 | 값 |
|------|------|
| 컨테이너명 | `rmq_broker`, `rmq_node_a`, `rmq_node_b` |
| compose 서비스명 | `rabbitmq`, `node_a`, `node_b` |
| 환경변수 `NODE_ID` | `A`, `B` (로그/큐명에 들어가는 짧은 식별자) |
| 큐 이름 (런타임 생성) | `sample.node.A`, `sample.node.B` |
| 네트워크 | `rmq_net` (broker compose 가 생성, sample compose 가 external join) |
| Exchange | `sample.fanout` (fanout 타입) |

## 전제

- 현재 working directory 가 프로젝트 루트(`d:\Projects\rabbitmq` 또는 동등)
- `docker/broker/docker-compose.yml`, `docker/sample/docker-compose.yml`, `Dockerfile` 이 존재
- Docker daemon 실행 중 (Docker Desktop 또는 Linux Docker Engine)
- 셸은 **bash** (PIPESTATUS / `/dev/tcp` 의존). Windows 에선 Git Bash / MSYS2 / WSL 사용
- 서브모듈 `third_party/libuv`, `third_party/AMQP-CPP` 가 체크아웃되어 있음

## 워크플로우

### Step 0a: Docker daemon 확인

먼저 daemon 살아 있는지 확인. 죽어 있으면 이후 명령이 모두 무의미한 에러를 양산하므로 여기서 중단.

```bash
docker info --format '{{.ServerVersion}}' || { echo "Docker daemon 미실행"; exit 1; }
```

### Step 0b: 이전 실행 잔재 정리

stale 컨테이너/네트워크가 남아 있으면 `Conflict. The container name "/rmq_broker" is already in use` 같은 에러로 Step 1 이 실패한다. 항상 먼저 정리한다(없을 땐 무해).

```bash
docker rm -f rmq_broker rmq_node_a rmq_node_b 2>/dev/null
docker compose -f docker/sample/docker-compose.yml down --remove-orphans 2>/dev/null
docker compose -f docker/broker/docker-compose.yml down --remove-orphans 2>/dev/null
docker network rm rmq_net 2>/dev/null
```

### Step 1: broker 기동

```bash
docker compose -f docker/broker/docker-compose.yml up -d
```

기대 출력 마지막 줄: `Container rmq_broker Started`. 부수적으로 `Network rmq_net Created` 도 나옴.

### Step 2: healthcheck 대기

`check_port_connectivity` 가 통과(=AMQP 5672 listen) 할 때까지 폴링. 첫 부팅은 약 12~18 초, 캐시된 이미지로 재기동 시 8~15 초.

```bash
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
    status=$(docker inspect --format='{{.State.Health.Status}}' rmq_broker 2>/dev/null)
    echo "  try $i: $status"
    if [ "$status" = "healthy" ]; then
        break
    fi
    sleep 3
done
```

`healthy` 도달 못 함 (12회 × 3초 = 36 초 후에도 `starting` / `unhealthy`):
- `docker logs rmq_broker | tail -50` 로 원인 확인
- 사용자에게 보고하고 Step 4 로 점프(정리)

### Step 3: sample(2-node) 기동 + 메시지 흐름 관찰

```bash
set -o pipefail  # 파이프 안 첫 명령의 exit code 가 보존되어 compose 실패 감지 가능
docker compose -f docker/sample/docker-compose.yml up --build \
    --abort-on-container-exit --exit-code-from node_a 2>&1 | tail -100
EXIT=$?
echo "EXIT=$EXIT"
```

각 옵션의 의미:
- `--build` — 소스가 바뀌었을 수 있어 항상 재빌드 시도. 캐시 hit 시 즉시 통과.
- `--abort-on-container-exit` — 한 컨테이너가 exit 하면 전체 compose 정리.
- `--exit-code-from node_a` — node_a 의 exit code 를 compose 전체의 exit code 로 채택.

기대 출력 (msg #N 은 `PUBLISH_COUNT=10` 까지 진행):

```
[A] PUB  [A] msg #1
[B]  RX  from=A : [A] msg #1
[A] SELF from=A : [A] msg #1
[B] PUB  [B] msg #1
[A]  RX  from=B : [B] msg #1
[B] SELF from=B : [B] msg #1
... (msg #10 까지 반복)
[A] publish done, linger 3000ms
[A] shutting down
[A] connection closed by broker
[A] exit
[B] signal 15 received
[B] shutting down
[B] connection closed by broker
[B] exit
EXIT=0
```

검증 포인트:
- `[A] PUB`, `[B] PUB` 가 각각 **10개**씩 발행되었는가
- 각 노드가 **자기 메시지 10개(`SELF`) + 상대 메시지 10개(` RX `) = 20 수신**인가
- 두 노드 모두 `exit` 로 graceful 종료했는가 (kill -9 가 아닌)
- `EXIT=0` 인가

### Step 4: broker teardown + 네트워크 정리

```bash
docker compose -f docker/broker/docker-compose.yml down
```

기대 출력: `Container rmq_broker Removed` + `Network rmq_net Removed`. (sample compose 는 Step 3 의 `--abort-on-container-exit` 가 이미 정리함)

### Step 5: 사용자에게 결과 보고

성공 시 (예시 — 실제 값은 측정값 사용):
```
✅ simulate-docker 통과
- broker:  rabbitmq:3-management, healthcheck 통과 (15s)
- node_a:  발행 10 / 수신 20 (self 10 + peer 10), exit 0
- node_b:  발행 10 / 수신 20 (self 10 + peer 10), exit 0
- 정리:    rmq_broker / rmq_net 모두 제거
```

실패 시: 어느 Step 에서 어떤 신호로 실패했는지 + 관련 로그 30 줄 정도를 첨부. 가능하면 아래 "알려진 실패 패턴" 표에서 해당 행을 찾아 함께 안내.

## 알려진 실패 패턴

| 증상 | 원인 | 조치 / 회복 |
|------|------|------------|
| `healthy` 도달 못 함 | RabbitMQ 자체 부팅 실패 (메모리/디스크 부족, 이미지 변경) | `docker logs rmq_broker` 마지막 50 줄 확인 → Step 4 실행해 정리 |
| 모든 메시지에 `Connection refused` | broker 의 healthcheck 가 `ping` 으로 잘못 설정 → 5672 listen 전 healthy 마킹 | `docker/broker/docker-compose.yml` 의 healthcheck `test:` 가 `check_port_connectivity` 인지 확인 |
| node 컨테이너가 linger 후에도 안 죽음 | `uv_signal_t` 가 종료 경로에서 close 안 됨 | `src/node/main.cpp` 의 `begin_shutdown` 이 `SigInt`/`SigTerm` 도 close 하는지 확인. 임시 회복: 다른 터미널에서 `docker kill rmq_node_a rmq_node_b` |
| `Conflict. The container name "/rmq_broker" is already in use` | 이전 실행의 stale container | Step 0b 가 이미 처리. 그래도 발생하면 `docker ps -a` 로 다른 이름 충돌 확인 |
| 빌드 실패 `format: No such file or directory` | base image 가 `ubuntu:22.04` (GCC 11, `<format>` 미존재) | `Dockerfile` 이 `FROM ubuntu:24.04` 인지 확인 |
| sample 시작 시 `network rmq_net declared as external, but could not be found` | broker compose 가 먼저 떠 있지 않음 | Step 1 + Step 2 가 끝났는지 확인. healthy 면 Step 3 재시도 |
| Windows 호스트에서 `bash`/`/dev/tcp` 오류 | Git Bash/MSYS2/WSL 이 아닌 cmd/PowerShell 로 실행 | Git Bash 또는 WSL 셸에서 재실행 |

## 변형

- **반복 검증(같은 소스)**: Step 0 ~ Step 4 를 그대로 반복하면 됨. 두 번째부터 docker 이미지 캐시가 살아 있어 보통 1분 이내.
- **메시지 양 늘려보기**: `docker/sample/docker-compose.yml` 의 `PUBLISH_COUNT=10` 을 더 큰 값(예: `100`)으로 일시 변경 후 Step 3 재실행. `PUBLISH_INTERVAL_MS` 도 함께 줄여 부하 테스트 가능.
- **무한 watch (개발 중 라이브 관찰)**: 컴파일 출력 점진 확인용. `--exit-code-from` / `--abort-on-container-exit` 를 빼고 `docker compose ... up` 만 실행. 단, 우리 `rmq_node` 는 `PUBLISH_COUNT` 만큼만 발행 후 linger → 자체 종료하므로 정말 "무한" 으로 두려면 `PUBLISH_COUNT=999999` 처럼 큰 값을 환경변수로 주입.
- **broker 만 띄우고 외부 도구로 검증**: 이 skill 의 본래 범위 밖. broker only 모드는 `docker compose -f docker/broker/docker-compose.yml up -d` 한 줄로 충분하므로 별도 skill 필요 없음.

## 비고

- 이 skill 은 본 프로젝트의 compose 구조에 강하게 결합되어 있다. 다른 프로젝트로 옮길 때는 컨테이너 이름, 네트워크 이름, 노드 ID, 로그 검증 패턴을 그 프로젝트에 맞게 수정.
- 첫 실행은 ubuntu:24.04 + RabbitMQ 이미지 다운로드 + 서브모듈 풀 컴파일이 동반되어 **3~5 분**. 두 번째부터는 캐시로 보통 **30~60 초**.
- 본 워크플로우의 디자인 결정 근거(예: `check_port_connectivity` 채택, `uv_signal_t` 종료 시 close 의 이유) 는 `CLAUDE.md` "핵심 디자인 결정" 절에 기록되어 있다 — 회귀 의심 시 참조.
