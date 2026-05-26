#include <uv.h>

#include <amqpcpp.h>
#include <amqpcpp/linux_tcp.h>   // IWYU: AMQP::TcpConnection / AMQP::TcpChannel 직접 사용

#include "uvx/core/log.hpp"

#include "env_helper.hpp"
#include "node_handler.hpp"
#include "node_runtime.hpp"

#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

// 기본값 — cpp-patterns: 상수는 `kPascalCase()` constexpr 함수 형태.
constexpr std::string_view kDefaultUrl()        { return "amqp://guest:guest@localhost/"; }
constexpr std::string_view kDefaultExchange()   { return "sample.fanout"; }
// "unset" 은 함정 회피용 sentinel — main 에서 검증하여 에러로 변환.
// 두 인스턴스가 동일한 NODE_ID 면 동일 큐를 공유해 work queue 패턴이 되어
// fanout 의도(양쪽 다 받기) 가 깨진다.
constexpr std::string_view kDefaultNodeId()     { return "unset"; }
constexpr int              kDefaultCount()      { return 10; }
constexpr uint64_t         kDefaultIntervalMs() { return 500; }
constexpr uint64_t         kDefaultLingerMs()   { return 3000; }

}  // namespace

int main() {
    try {
        const auto url           = env_or("RABBITMQ_URL", kDefaultUrl());
        const auto exchange      = env_or("RABBITMQ_EXCHANGE", kDefaultExchange());
        const auto node_id       = env_or("NODE_ID", kDefaultNodeId());
        const auto publish_count = env_int_or("PUBLISH_COUNT", kDefaultCount());
        const auto interval_ms   = env_u64_or("PUBLISH_INTERVAL_MS", kDefaultIntervalMs());
        const auto linger_ms     = env_u64_or("LINGER_MS", kDefaultLingerMs());

        // NODE_ID 미설정 시 즉시 fail-fast (work queue 함정 회피).
        if (node_id == kDefaultNodeId()) {
            throw std::runtime_error(
                "NODE_ID 환경변수 필수. 예: NODE_ID=A ./rmq_node "
                "(두 인스턴스를 띄울 땐 서로 다른 값으로)");
        }

        uvx::log::info(
            "[{}] start: url={} exchange={} count={} interval={}ms linger={}ms",
            node_id, url, exchange, publish_count, interval_ms, linger_ms);

        auto* loop = uv_default_loop();

        NodeHandler         handler(loop);
        AMQP::TcpConnection connection(&handler, AMQP::Address(url));
        AMQP::TcpChannel    channel(&connection);

        // NodeContext 는 heap — callback 사이로 raw 포인터로 안전 전달.
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

        start_node(loop, node_ctx, interval_ms);

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
