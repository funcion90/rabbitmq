#pragma once

#include <uv.h>

#include <amqpcpp.h>
#include <amqpcpp/linux_tcp.h>   // AMQP::TcpChannel / AMQP::TcpConnection

#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// NodeContext — callback 간 공유되는 노드 상태.
// struct 기본 access 가 public 이므로 멤버는 PascalCase (cpp-patterns 규약).
// 수명은 main 이 heap 으로 관리(`new` → uv_run 종료 후 `delete`).
// ---------------------------------------------------------------------------
struct NodeContext {
    AMQP::TcpChannel*    Channel;
    AMQP::TcpConnection* Connection;
    uv_timer_t*          PublishTimer;
    uv_timer_t*          LingerTimer;
    uv_signal_t*         SigInt;
    uv_signal_t*         SigTerm;
    std::string          Exchange;
    std::string          NodeId;
    int                  PublishCount;
    int                  Sent;
    uint64_t             LingerMs;
    bool                 ShutdownStarted;
};

// ---------------------------------------------------------------------------
// start_node — 노드를 본격 가동시키는 단일 진입점.
//
// 수행 작업 순서:
//   1) exchange 선언 (fanout, durable)
//   2) queue 선언 (`sample.node.<NodeId>`) + exchange 바인딩
//   3) channel.consume() + onReceived/onSuccess/onError 람다 등록
//   4) publish 타이머 init + start (in_interval_ms 주기)
//   5) linger 타이머 init (시작 안 함 — on_publish_tick 에서 1-shot 발동)
//   6) SIGINT/SIGTERM 핸들러 init + start
//
// 모든 libuv 핸들 포인터는 in_ctx 에 채워져 begin_shutdown 단계에서 일괄 close.
// 호출 후 호출자(main)가 uv_run 진입.
//
//   - in_loop:        uv_default_loop() 결과
//   - in_channel:     이미 connection 과 묶인 채널 (수명은 호출자가 보장)
//   - in_ctx:         heap 으로 할당된 NodeContext, 위 작업으로 채워짐
//   - in_interval_ms: publish 타이머 간격
// ---------------------------------------------------------------------------
void start_node(uv_loop_t*        in_loop,
                AMQP::TcpChannel* in_channel,
                NodeContext*      in_ctx,
                uint64_t          in_interval_ms);
