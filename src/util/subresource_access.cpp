#include "util/subresource_access.h"

#include "util/remote_asset.h"

#include <atomic>
#include <filesystem>
#include <system_error>

namespace bro::util {

namespace {
std::atomic<SubresourceGate> g_gate{nullptr};
}  // namespace

void setSubresourceGate(SubresourceGate gate) {
    g_gate.store(gate, std::memory_order_release);
}

std::string subresourceRefusal(const std::string& pathOrUrl) {
    SubresourceGate gate = g_gate.load(std::memory_order_acquire);
    if (!gate) return {};
    if (isHttpUrl(pathOrUrl)) return gate(true, pathOrUrl);
    namespace fs = std::filesystem;
    const fs::path p(std::u8string(pathOrUrl.begin(), pathOrUrl.end()));
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec) abs = p;
    const std::u8string u = abs.lexically_normal().u8string();
    return gate(false, std::string(u.begin(), u.end()));
}

}  // namespace bro::util
