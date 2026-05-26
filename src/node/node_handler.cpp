#include "node_handler.hpp"

#include "uvx/core/log.hpp"

void NodeHandler::onError(AMQP::TcpConnection* /*in_connection*/, const char* in_message) {
    uvx::log::error("amqp error: {}", in_message);
}

void NodeHandler::onConnected(AMQP::TcpConnection* /*in_connection*/) {
    uvx::log::info("connected to broker");
}

void NodeHandler::onClosed(AMQP::TcpConnection* /*in_connection*/) {
    uvx::log::info("connection closed by broker");
}
