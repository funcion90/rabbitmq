#include <uv.h>

#include <amqpcpp.h>
#include <amqpcpp/libuv.h>

#include "uvx/core/log.hpp"
#include "uvx/core/uv_check.hpp"

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

// ---------------------------------------------------------------------------
// 기본값 — cpp-patterns: 상수는 `kPascalCase()` constexpr 함수 형태.
// ---------------------------------------------------------------------------
constexpr std::string_view kDefaultUrl()        { return "amqp://guest:guest@localhost/"; }
constexpr std::string_view kDefaultExchange()   { return "sample.fanout"; }
// "unset" 은 함정 회피용 sentinel — main 에서 검증하여 에러로 변환.
// 의도된 동작: 두 인스턴스를 띄울 땐 반드시 NODE_ID 를 각자 다르게 지정해야
// 큐가 분리되어 양방향 fanout 이 의도대로 동작한다. 같은 ID 면 work queue 가 됨.
constexpr std::string_view kDefaultNodeId()     { return "unset"; }
constexpr int              kDefaultCount()      { return 10; }
constexpr uint64_t         kDefaultIntervalMs() { return 500; }
constexpr uint64_t         kDefaultLingerMs()   { return 3000; }

// ---------------------------------------------------------------------------
// env 헬퍼 — input parameter 는 `in_` 접두사 (cpp-patterns 규약).
// ---------------------------------------------------------------------------
[[nodiscard]] std::string env_or(const char* in_name, std::string_view in_fallback) {
    const char* v = std::getenv(in_name);
    if (nullptr == v) {
        return std::string(in_fallback);
    }
    return std::string(v);
}

[[nodiscard]] int env_int_or(const char* in_name, int in_fallback) {
    const char* v = std::getenv(in_name);
    if (nullptr == v) {
        return in_fallback;
    }
    try {
        return std::stoi(v);
    } catch (...) {
        return in_fallback;
    }
}

[[nodiscard]] uint64_t env_u64_or(const char* in_name, uint64_t in_fallback) {
    const char* v = std::getenv(in_name);
    if (nullptr == v) {
        return in_fallback;
    }
    try {
        return static_cast<uint64_t>(std::stoull(v));
    } catch (...) {
        return in_fallback;
    }
}

// ---------------------------------------------------------------------------
// NodeHandler — AMQP::LibUvHandler 상속.
// override 메서드명은 base virtual 의 시그니처라 camelCase 고정 (외부 강제).
// ---------------------------------------------------------------------------
class NodeHandler : public AMQP::LibUvHandler {
public:
    using AMQP::LibUvHandler::LibUvHandler;

private:
    void onError(AMQP::TcpConnection* /*in_connection*/, const char* in_message) override {
        uvx::log::error("amqp error: {}", in_message);
    }

    void onConnected(AMQP::TcpConnection* /*in_connection*/) override {
        uvx::log::info("connected to broker");
    }

    void onClosed(AMQP::TcpConnection* /*in_connection*/) override {
        uvx::log::info("connection closed by broker");
    }
};

// ---------------------------------------------------------------------------
// NodeContext — 콜백 간 공유 상태. struct 기본 access 가 public 이므로
// 멤버는 PascalCase (cpp-patterns: public 멤버 규약).
// ---------------------------------------------------------------------------
struct NodeContext {
    AMQP::TcpChannel*    Channel;
    AMQP::TcpConnection* Connection;
    uv_timer_t*          PublishTimer;
    uv_timer_t*          LingerTimer;
    uv_signal_t*         SigInt;       // 종료 시 같이 close 해야 loop 가 빠진다
    uv_signal_t*         SigTerm;
    std::string          Exchange;
    std::string          NodeId;
    int                  PublishCount;
    int                  Sent;
    uint64_t             LingerMs;
    bool                 ShutdownStarted;
};

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
// Publish timer tick — interval 마다 1 메시지 발행.
// PublishCount 도달 시 linger 로 전환.
// ---------------------------------------------------------------------------
void on_publish_tick(uv_timer_t* in_timer) noexcept {
    auto* node = reinterpret_cast<NodeContext*>(in_timer->data);

    const auto payload = std::format("[{}] msg #{}", node->NodeId, node->Sent + 1);

    AMQP::Envelope envelope(payload.data(), payload.size());
    AMQP::Table    headers;
    headers.set("sender", node->NodeId);
    envelope.setHeaders(std::move(headers));  // Table&& 오버로드로 복사 회피

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
// 실제 핸들 close 는 begin_shutdown 안에서 일괄 처리.
// ---------------------------------------------------------------------------
void on_signal(uv_signal_t* in_sig, int in_signum) noexcept {
    auto* node = reinterpret_cast<NodeContext*>(in_sig->data);
    uvx::log::info("[{}] signal {} received", node->NodeId, in_signum);
    begin_shutdown(node);
}

}  // namespace

int main() {
    try {
        const auto url           = env_or("RABBITMQ_URL", kDefaultUrl());
        const auto exchange      = env_or("RABBITMQ_EXCHANGE", kDefaultExchange());
        const auto node_id       = env_or("NODE_ID", kDefaultNodeId());
        const auto publish_count = env_int_or("PUBLISH_COUNT", kDefaultCount());
        const auto interval_ms   = env_u64_or("PUBLISH_INTERVAL_MS", kDefaultIntervalMs());
        const auto linger_ms     = env_u64_or("LINGER_MS", kDefaultLingerMs());

        // NODE_ID 미설정 함정 차단: 같은 ID 두 인스턴스 = 같은 큐 = work queue 패턴
        // (fanout 의도와 다르게 메시지가 라운드로빈 분배되어 한쪽만 받음).
        if (node_id == kDefaultNodeId()) {
            throw std::runtime_error(
                "NODE_ID 환경변수 필수. 예: NODE_ID=A ./rmq_node "
                "(두 인스턴스를 띄울 땐 서로 다른 값으로)");
        }

        const auto queue_name = std::format("sample.node.{}", node_id);

        uvx::log::info(
            "[{}] start: url={} exchange={} queue={} count={} interval={}ms linger={}ms",
            node_id, url, exchange, queue_name, publish_count, interval_ms, linger_ms);

        auto* loop = uv_default_loop();

        NodeHandler         handler(loop);
        AMQP::TcpConnection connection(&handler, AMQP::Address(url));
        AMQP::TcpChannel    channel(&connection);

        // fanout 이라 routing key 무의미. 큐는 노드별 고유.
        channel.declareExchange(exchange, AMQP::fanout, AMQP::durable);
        channel.declareQueue(queue_name, AMQP::durable);
        channel.bindQueue(exchange, queue_name, "");

        // 컨텍스트 — heap 으로 두어 callback 사이로 안전 전달.
        auto* node_ctx = new NodeContext{
            .Channel         = &channel,
            .Connection      = &connection,
            .PublishTimer    = nullptr,
            .LingerTimer     = nullptr,
            .SigInt          = nullptr,
            .SigTerm         = nullptr,
            .Exchange        = exchange,
            .NodeId          = node_id,
            .PublishCount    = publish_count,
            .Sent            = 0,
            .LingerMs        = linger_ms,
            .ShutdownStarted = false,
        };

        // 구독 — 자기 큐. sender 헤더로 자기/타인 구분 표시.
        channel.consume(queue_name, AMQP::noack)
            .onReceived([node_ctx](const AMQP::Message& in_msg, uint64_t /*in_tag*/, bool /*in_redelivered*/) {
                const auto              body   = std::string_view(in_msg.body(), in_msg.bodySize());
                const std::string&      sender = in_msg.headers().get("sender");
                const std::string_view  kind   = (sender == node_ctx->NodeId) ? "SELF" : " RX ";
                uvx::log::info("[{}] {} from={} : {}", node_ctx->NodeId, kind, sender, body);
            })
            .onSuccess([node_id](const std::string& in_tag) {
                uvx::log::info("[{}] consumer ready, tag={}", node_id, in_tag);
            })
            .onError([node_id](const char* in_message) {
                uvx::log::error("[{}] consume error: {}", node_id, in_message);
            });

        // Publish timer — interval 마다 발행.
        auto* publish_timer = new uv_timer_t{};
        uvx::check(uv_timer_init(loop, publish_timer), "uv_timer_init(publish)");
        publish_timer->data = node_ctx;
        node_ctx->PublishTimer = publish_timer;
        uvx::check(uv_timer_start(publish_timer, on_publish_tick, interval_ms, interval_ms),
                   "uv_timer_start(publish)");

        // Linger timer — 초기엔 idle. 발행 끝나면 on_publish_tick 에서 1-shot start.
        auto* linger_timer = new uv_timer_t{};
        uvx::check(uv_timer_init(loop, linger_timer), "uv_timer_init(linger)");
        linger_timer->data = node_ctx;
        node_ctx->LingerTimer = linger_timer;

        // Signal handler — 외부 종료 신호 처리.
        auto* sig_int  = new uv_signal_t{};
        auto* sig_term = new uv_signal_t{};
        uvx::check(uv_signal_init(loop, sig_int),  "uv_signal_init(SIGINT)");
        uvx::check(uv_signal_init(loop, sig_term), "uv_signal_init(SIGTERM)");

        sig_int->data  = node_ctx;
        sig_term->data = node_ctx;
        node_ctx->SigInt  = sig_int;
        node_ctx->SigTerm = sig_term;

        uvx::check(uv_signal_start(sig_int,  on_signal, SIGINT),  "uv_signal_start(SIGINT)");
        uvx::check(uv_signal_start(sig_term, on_signal, SIGTERM), "uv_signal_start(SIGTERM)");

        const int run_rc = uv_run(loop, UV_RUN_DEFAULT);
        if (0 != run_rc) {
            uvx::log::warn("[{}] uv_run returned {}", node_id, run_rc);
        }

        delete node_ctx;
        uv_loop_close(loop);
        uvx::log::info("[{}] exit", node_id);
        return 0;
    } catch (const std::exception& e) {
        uvx::log::error("fatal: {}", e.what());
        return 1;
    }
}
