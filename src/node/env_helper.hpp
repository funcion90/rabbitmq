#pragma once

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// 환경변수 읽기 헬퍼 — input parameter 는 `in_` 접두사 (cpp-patterns).
// 모두 inline 헤더-온리: main.cpp 외엔 사용처가 없지만, 향후 다른 노드가 생기면
// 그대로 include 해서 쓰도록 헤더 분리.
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::string env_or(const char* in_name, std::string_view in_fallback) {
    const char* v = std::getenv(in_name);
    if (nullptr == v) {
        return std::string(in_fallback);
    }
    return std::string(v);
}

[[nodiscard]] inline int env_int_or(const char* in_name, int in_fallback) {
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

[[nodiscard]] inline uint64_t env_u64_or(const char* in_name, uint64_t in_fallback) {
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
