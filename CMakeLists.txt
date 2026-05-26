cmake_minimum_required(VERSION 3.20)

project(rabbitmq_sample
    LANGUAGES CXX
    DESCRIPTION "libuv + AMQP-CPP based RabbitMQ sample (publisher / consumer)"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
    add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# AMQP-CPP 의 linux_tcp 모듈은 POSIX 전용(sys/socket, poll, dlopen, OpenSSL).
# 네이티브 Windows 빌드는 지원하지 않는다 — Docker(권장) 또는 WSL 사용.
# macOS 는 POSIX 라 컴파일 자체는 시도되지만, OpenSSL dlopen 의 dylib 경로 차이로
# 런타임 에러가 발생할 수 있다. 검증된 환경은 Linux / Docker / WSL 만이다.
# ---------------------------------------------------------------------------
if(WIN32 AND NOT CYGWIN)
    message(FATAL_ERROR
        "Native Windows build is not supported.\n"
        "Use the provided Dockerfile / docker-compose, or build under WSL.\n"
        "Reason: AMQP-CPP linux_tcp module uses POSIX-only APIs.")
endif()

if(APPLE)
    message(WARNING
        "macOS native build is untested. AMQP-CPP linux_tcp uses dlopen(libssl) "
        "which may fail to find OpenSSL dylibs on macOS. "
        "If you hit OpenSSL runtime errors, use the provided Docker workflow instead.")
endif()

# ---------------------------------------------------------------------------
# 서브모듈 1: funcion90/libuv
#   - third_party/libuv/CMakeLists.txt 가 FetchContent 로 libuv v1.52.1 본체를
#     내려받고, `uvx_common` INTERFACE 라이브러리(헤더 + uv_a 링크) 를 제공한다.
#   - EXCLUDE_FROM_ALL 로 그쪽의 echo_server / echo_client 샘플은 기본 빌드에서 제외.
# ---------------------------------------------------------------------------
add_subdirectory(third_party/libuv EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# 서브모듈 2: AMQP-CPP (v4.3.27)
#   - linux_tcp 옵션을 켜야 TcpConnection / TcpHandler 가 빌드된다.
#   - 정적 라이브러리로 빌드하여 단일 바이너리 산출을 지향.
# ---------------------------------------------------------------------------
set(AMQP-CPP_LINUX_TCP     ON  CACHE BOOL "" FORCE)
set(AMQP-CPP_BUILD_SHARED  OFF CACHE BOOL "" FORCE)
set(AMQP-CPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/AMQP-CPP EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# 샘플 타겟 — 단일 바이너리.
#   동일 바이너리가 NODE_ID 별로 fanout exchange 에 publish + 자기 큐 subscribe 를
#   모두 수행한다. 2 인스턴스 띄우면 서로 메시지 주고받는 ping-pong 데모.
#
# 구조:
#   main.cpp           — env 로딩 + AMQP 연결/채널/핸들러 생성 + start_node 호출 + uv_run
#   env_helper.hpp     — env_or / env_int_or / env_u64_or (헤더 onlly)
#   node_handler.cpp   — NodeHandler (AMQP::LibUvHandler 상속, 연결 이벤트 로깅)
#   node_runtime.cpp   — NodeContext 상태 + libuv 콜백들 + start_node 본체
# ---------------------------------------------------------------------------
add_executable(rmq_node
    src/node/main.cpp
    src/node/node_handler.cpp
    src/node/node_runtime.cpp
)
target_link_libraries(rmq_node PRIVATE uvx_common amqpcpp)
