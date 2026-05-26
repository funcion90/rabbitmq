#pragma once

#include <amqpcpp.h>
#include <amqpcpp/libuv.h>

// ---------------------------------------------------------------------------
// AMQP-CPP TCP 연결 이벤트(connected/error/closed) 를 받아 uvx::log 로 출력하는
// 단순 핸들러. 생성자는 base 의 (uv_loop_t*) 시그니처를 그대로 노출.
// override 메서드 이름은 base virtual 시그니처 (camelCase) 외부 강제.
// ---------------------------------------------------------------------------
class NodeHandler : public AMQP::LibUvHandler {
public:
    using AMQP::LibUvHandler::LibUvHandler;

private:
    void onError(AMQP::TcpConnection* in_connection, const char* in_message) override;
    void onConnected(AMQP::TcpConnection* in_connection) override;
    void onClosed(AMQP::TcpConnection* in_connection) override;
};
