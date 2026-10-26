#include "util/subresource_access.h"

#include <atomic>
#include <filesystem>
#include <system_error>

namespace bro::util {

namespace {
std::atomic<SubresourceReadGate> g_gate{nullptr};
}  // namespace

void setSubresourceReadGate(SubresourceReadGate gate) {
    g_gate.store(gate, std::memory_order_release);
}

std::string subresourceReadRefusal(const std::string& path) {
    SubresourceReadGate gate = g_gate.load(std::memory_order_acquire);
    if (!gate) return {};
    namespace fs = std::filesystem;
    const fs::path p(std::u8string(path.begin(), path.end()));
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec) abs = p;
    const std::u8string u = abs.lexically_normal().u8string();
    return gate(std::string(u.begin(), u.end()));
}

}  // namespace bro::util
