#include "node_runtime.hpp"

#include <uv.h>

#include <amqpcpp.h>

#include "uvx/core/log.hpp"
#include "uvx/core/uv_check.hpp"

#include <csignal>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace {

// ---------------------------------------------------------------------------
// libuv handle 정리 콜백 (anonymous namespace, noexcept).
// ---------------------------------------------------------------------------
void on_close_timer(uv_handle_t* in_handle) noexcept {
    delete reinterpret_cast<uv_timer_t*>(in_handle);
}

void on_close_signal(uv_handle_t* in_handle) noexcept {
    delete reinterpret_cast<uv_signal_t*>(in_handle);
}

void close_handle_if_open(uv_handle_t* in_handle, uv_close_cb in_cb) noexcept {
    if (nullptr == in_handle) {
        return;
    }
    if (false == uv_is_closing(in_handle)) {
        uv_close(in_handle, in_cb);
    }
}

// ---------------------------------------------------------------------------
// 종료 절차 — 어디서 호출되든 한 번만 실행. AMQP 채널/커넥션 close 후
// libuv watcher 가 자동 unregister 되어 uv_run 자연 종료.
// ---------------------------------------------------------------------------
void begin_shutdown(NodeContext* in_node) noexcept {
    if (true == in_node->ShutdownStarted) {
        return;
    }
    in_node->ShutdownStarted = true;
    uvx::log::info("[{}] shutting down", in_node->NodeId);

    // 시그널 핸들은 active handle 로 카운트되므로 loop 자연 종료를 위해 함께 close.
    close_handle_if_open(reinterpret_cast<uv_handle_t*>(in_node->SigInt),  on_close_signal);
    close_handle_if_open(reinterpret_cast<uv_handle_t*>(in_node->SigTerm), on_close_signal);
    in_node->SigInt  = nullptr;
    in_node->SigTerm = nullptr;

    auto* conn = in_node->Connection;
    in_node->Channel->close().onFinalize([conn]() noexcept {
        conn->close();
    });
}

// ---------------------------------------------------------------------------
// Linger timer 만료 — in-flight 메시지 수신 여유 끝, 종료 진입.
// ---------------------------------------------------------------------------
void on_linger_expire(uv_timer_t* in_timer) noexcept {
    auto* node = reinterpret_cast<NodeContext*>(in_timer->data);
    uv_timer_stop(in_timer);
    close_handle_if_open(reinterpret_cast<uv_handle_t*>(in_timer), on_close_timer);
    node->LingerTimer = nullptr;
    begin_shutdown(node);
}

// ---------------------------------------------------------------------------
// Publish timer tick — interval 마다 1 메시지 발행. PublishCount 도달 시 linger 로 전환.
// ---------------------------------------------------------------------------
void on_publish_tick(uv_timer_t* in_timer) noexcept {
    auto* node = reinterpret_cast<NodeContext*>(in_timer->data);

    const auto payload = std::format("[{}] msg #{}", node->NodeId, node->Sent + 1);

    AMQP::Envelope envelope(payload.data(), payload.size());
    AMQP::Table    headers;
    headers.set("sender", node->NodeId);
    envelope.setHeaders(std::move(headers));

    const bool ok = node->Channel->publish(node->Exchange, "", envelope);
    if (false == ok) {
        uvx::log::warn("[{}] publish failed (channel down)", node->NodeId);
    } else {
        uvx::log::info("[{}] PUB {}", node->NodeId, payload);
    }

    ++node->Sent;
    if (node->Sent < node->PublishCount) {
        return;
    }

    // 발행 끝. publish timer 닫고 linger timer 를 단발(1-shot) 로 시작.
    uv_timer_stop(in_timer);
    close_handle_if_open(reinterpret_cast<uv_handle_t*>(in_timer), on_close_timer);
    node->PublishTimer = nullptr;

    if (nullptr == node->LingerTimer) {
        begin_shutdown(node);
        return;
    }

    uvx::log::info("[{}] publish done, linger {}ms", node->NodeId, node->LingerMs);
    const int rc = uv_timer_start(node->LingerTimer, on_linger_expire,
                                  node->LingerMs, /*repeat*/ 0);
    if (0 != rc) {
        uvx::log::error("[{}] linger timer start failed: {}", node->NodeId, uv_strerror(rc));
        begin_shutdown(node);
    }
}

// ---------------------------------------------------------------------------
// Signal handler — SIGINT/SIGTERM 즉시 graceful shutdown.
// ---------------------------------------------------------------------------
void on_signal(uv_signal_t* in_sig, int in_signum) noexcept {
    auto* node = reinterpret_cast<NodeContext*>(in_sig->data);
    uvx::log::info("[{}] signal {} received", node->NodeId, in_signum);
    begin_shutdown(node);
}

}  // namespace

// ---------------------------------------------------------------------------
// 공개 entry — node_runtime.hpp 의 start_node 정의.
// 채널은 in_ctx->Channel 에서만 참조 — 매개변수 중복 제거로 inconsistency 차단.
// ---------------------------------------------------------------------------
void start_node(uv_loop_t*   in_loop,
                NodeContext* in_ctx,
                uint64_t     in_interval_ms) {
    auto* const channel    = in_ctx->Channel;
    const auto  queue_name = std::format("sample.node.{}", in_ctx->NodeId);

    uvx::log::info("[{}] declaring exchange={} queue={}", in_ctx->NodeId,
                   in_ctx->Exchange, queue_name);

    // fanout 이라 routing key 무의미. 큐는 노드별 고유.
    channel->declareExchange(in_ctx->Exchange, AMQP::fanout, AMQP::durable);
    channel->declareQueue(queue_name, AMQP::durable);
    channel->bindQueue(in_ctx->Exchange, queue_name, "");

    // 구독 — 자기 큐. sender 헤더로 자기/타인 구분 표시.
    channel->consume(queue_name, AMQP::noack)
        .onReceived([in_ctx](const AMQP::Message& in_msg, uint64_t /*in_tag*/, bool /*in_redelivered*/) {
            const auto              body   = std::string_view(in_msg.body(), in_msg.bodySize());
            const std::string&      sender = in_msg.headers().get("sender");
            const std::string_view  kind   = (sender == in_ctx->NodeId) ? "SELF" : " RX ";
            uvx::log::info("[{}] {} from={} : {}", in_ctx->NodeId, kind, sender, body);
        })
        .onSuccess([node_id = in_ctx->NodeId](const std::string& in_tag) {
            uvx::log::info("[{}] consumer ready, tag={}", node_id, in_tag);
        })
        .onError([node_id = in_ctx->NodeId](const char* in_message) {
            uvx::log::error("[{}] consume error: {}", node_id, in_message);
        });

    // Publish timer — interval 마다 발행.
    auto* publish_timer = new uv_timer_t{};
    uvx::check(uv_timer_init(in_loop, publish_timer), "uv_timer_init(publish)");
    publish_timer->data    = in_ctx;
    in_ctx->PublishTimer   = publish_timer;
    uvx::check(uv_timer_start(publish_timer, on_publish_tick, in_interval_ms, in_interval_ms),
               "uv_timer_start(publish)");

    // Linger timer — 초기엔 idle. 발행 끝나면 on_publish_tick 에서 1-shot start.
    auto* linger_timer = new uv_timer_t{};
    uvx::check(uv_timer_init(in_loop, linger_timer), "uv_timer_init(linger)");
    linger_timer->data    = in_ctx;
    in_ctx->LingerTimer   = linger_timer;

    // Signal handler — 외부 종료 신호 처리.
    auto* sig_int  = new uv_signal_t{};
    auto* sig_term = new uv_signal_t{};
    uvx::check(uv_signal_init(in_loop, sig_int),  "uv_signal_init(SIGINT)");
    uvx::check(uv_signal_init(in_loop, sig_term), "uv_signal_init(SIGTERM)");

    sig_int->data    = in_ctx;
    sig_term->data   = in_ctx;
    in_ctx->SigInt   = sig_int;
    in_ctx->SigTerm  = sig_term;

    uvx::check(uv_signal_start(sig_int,  on_signal, SIGINT),  "uv_signal_start(SIGINT)");
    uvx::check(uv_signal_start(sig_term, on_signal, SIGTERM), "uv_signal_start(SIGTERM)");
}
