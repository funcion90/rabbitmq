# CLAUDE.md

`rabbitmq-sample` 프로젝트의 Claude Code 진입점 문서.

---

## 프로젝트 개요

| 항목 | 내용 |
|------|------|
| 종류 | libuv + AMQP-CPP 기반 C++20 ping-pong 노드 샘플 (RabbitMQ 통신) |
| 핵심 토폴로지 | 동일 `rmq_node` 바이너리 두 인스턴스가 fanout exchange 로 메시지 교환 |
| 의존성 (submodule) | `third_party/libuv` (funcion90/libuv, main), `third_party/AMQP-CPP` (v4.3.27) |
| 빌드 시스템 | CMake 3.20+, C++20 |
| 베이스 이미지 | `ubuntu:24.04` (GCC 13 — libstdc++ 의 `std::format` 사용) |
| 진입점 | `src/node/main.cpp` |
| 재사용 헤더 | `uvx::log::info/warn/error`, `uvx::check`, `uvx::err_message` (서브모듈 `funcion90/libuv` 제공) |

---

## 작업 전 필수 확인

1. **코드 패턴**: `third_party/libuv/.claude/rules/cpp-patterns.md` 우선 검토 — 본 프로젝트도 그대로 적용 (인코딩, Yoda, 캐스트, 정렬, in_/out_ prefix, 콜백 noexcept).
2. **기존 패턴 답습**: `src/node/main.cpp` — env 헬퍼, NodeHandler, NodeContext (public 멤버 PascalCase), libuv 핸들 수명 관리 패턴이 다 거기에 있다.
3. **AMQP-CPP API 확인**: 새 API 쓰기 전에 `third_party/AMQP-CPP/include/amqpcpp/` 안의 헤더를 직접 읽어 시그니처/시맨틱 확인 (특히 `Field` implicit conversion, `Deferred::onSuccess/onFinalize` 체이닝).

---

## 필수 규칙

- **언어**: 모든 응답은 한글로 작성
- **C++ 파일**: UTF-8 with BOM, CRLF (`*.cpp`, `*.hpp`, `*.h`, `*.inl` 전체 — cpp-patterns 강제)
- **Bash (Git Bash / MSYS2)**:
  - Windows 옵션의 `/` 는 **`//` 로 이스케이프** (예: `cmd //c "..."`, `dir //b`)
  - Windows 명령(`dir`, `del`, `type` 등)은 `cmd //c "명령어"` 로 실행
  - 가능하면 Unix 명령어(`ls`, `cat`, `grep`) 우선 사용
- **Python**: `python3` 금지 → 반드시 `python` 사용
- **네이티브 Windows 빌드 금지**: AMQP-CPP linux_tcp 모듈이 POSIX 전용. CMake 가 Windows native 에선 `FATAL_ERROR` 로 즉시 중단된다. Docker 또는 WSL 사용.

---

## 프로젝트 구조

```
D:\Projects\rabbitmq\
├── CMakeLists.txt              # rmq_node 단일 타겟. libuv/AMQP-CPP 서브모듈 add_subdirectory.
├── Dockerfile                  # ubuntu:24.04 멀티스테이지 빌드.
├── docker/
│   ├── broker/
│   │   └── docker-compose.yml  # RabbitMQ broker 단독. named network 'rmq_net' 생성.
│   └── sample/
│       └── docker-compose.yml  # node_a, node_b 두 인스턴스. external rmq_net 에 join.
├── src/
│   └── node/main.cpp           # pub+sub 통합 바이너리.
├── third_party/
│   ├── libuv/                  # 서브모듈: funcion90/libuv (uvx::* 유틸 + libuv 본체 FetchContent)
│   └── AMQP-CPP/               # 서브모듈: AMQP-CPP v4.3.27 (linux_tcp + LibUvHandler)
├── README.md
└── .gitmodules / .gitignore / .gitattributes / .dockerignore
```

---

## 핵심 디자인 결정 — 기록용

1. **fanout exchange + per-instance queue** — 두 노드가 서로의 메시지를 모두 받게 하려면 fanout 필수. work queue 패턴(direct + 공유 큐)이면 라운드로빈 분배되어 한쪽만 받는다.
2. **`AMQP::noack` 구독** — 학습/데모 단순화 목적. 운영 코드는 명시적 ack + `setQos(prefetch)` 패턴이 필요.
3. **`std::format` 의존 → ubuntu:24.04 강제** — GCC 11(libstdc++ 12)는 `<format>` 헤더 자체가 없어 22.04 에서 빌드 실패. GCC 13(libstdc++ 13) 이상이 요구된다.
4. **healthcheck = `check_port_connectivity`** — `rabbitmq-diagnostics ping` 은 Erlang VM 만 검사하여 AMQP 5672 가 listen 되기 전에 healthy 로 마킹된다. 클라이언트가 그 사이 connect → "Connection refused" → AMQP-CPP 채널이 error state 로 영구 잠긴다. `check_port_connectivity` 가 실제 listener accept 가능성을 검증한다.
5. **compose 간 통신 = named external network** — broker compose 가 `name: rmq_net` 으로 생성, sample compose 가 `external: true` 로 join. `RABBITMQ_URL=amqp://...@rabbitmq:5672/` 가 양쪽 DNS 에서 동일하게 풀린다.

---

## 코드 작성 규칙 인덱스

| 항목 | 위치 |
|------|------|
| C++ 패턴 (인코딩/캐스트/Yoda/정렬/네이밍/주석) | `third_party/libuv/.claude/rules/cpp-patterns.md` |
| MD 파일명 규약 | `third_party/libuv/.claude/rules/md-patterns.md` |

cpp-patterns 가장 자주 위반되는 항목 체크리스트:

- [ ] public struct 멤버는 `PascalCase` (예: `NodeContext::Channel`)
- [ ] 입력 인자에 `in_` prefix (예: `in_message`, `in_handle`)
- [ ] `if (nullptr == ptr)` Yoda 비교 (`!ptr` 금지)
- [ ] libuv 콜백은 `noexcept` + 익명 namespace
- [ ] `reinterpret_cast<uv_handle_t*>(...)` (C-style cast 금지)
- [ ] 연속 변수 선언의 `=` 세로 정렬

---

## 빌드 / 실행

```bash
# 풀빌드 (Linux/WSL)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel --target rmq_node

# Docker 권장 경로
docker compose -f docker/broker/docker-compose.yml up -d
docker compose -f docker/sample/docker-compose.yml up --build \
    --abort-on-container-exit --exit-code-from node_a
docker compose -f docker/broker/docker-compose.yml down
```

---

## 주의사항

- **libuv 본체 수정 금지**: `third_party/libuv/build/_deps/libuv-src/` 는 FetchContent 가 받아온 외부 코드. 직접 수정 금지.
- **AMQP-CPP 본체 수정 금지**: `third_party/AMQP-CPP/` 도 서브모듈. 본 프로젝트에서 수정하지 말고 upstream PR 권장.
- **libuv C API 경계**: 콜백 시그니처의 `int`, `ssize_t`, `unsigned int`, `uv_buf_t` 는 **외부 타입 그대로** 받아쓴다.
- **콜백은 `noexcept`**: libuv 콜백 내부 예외 누출은 미정의 동작. 예외 처리는 `main()` 경계에서만.
- **AMQP-CPP 채널 에러 영구성**: 한 번 error state 로 들어간 채널은 재사용 불가 — `TcpConnection` 자체를 새로 만들어야 한다.

---

## 버전 관리

- **Git** 사용. 원격: `https://github.com/funcion90/rabbitmq.git`
- 서브모듈 업데이트는 명시적 (`git -C third_party/libuv pull origin main` → `git add third_party/libuv`).
- AMQP-CPP 는 stable 태그(`v4.x.y`) 에 핀. 자동 master 추적 금지.
- `build/`, `_deps/` 는 `.gitignore` 에 포함.
- **커밋 전 사용자 확인 필수** — 자동 커밋 금지.

---

## Plan 모드 규칙

계획 파일 작성 시 다음 항목 필수:

- 사용자 원본 요청 (수정 없이 그대로 인용)
- 영향받는 범위 (파일/디렉토리)
- 검색 가능한 핵심 키워드 3~5개
- 요구사항 분해 목록
- 검증 방법 (실행/관찰 가능 단계)
