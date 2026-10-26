// bro's own natives asking the host's access gate (host_access.h).

#include "bronze_host/host_access.h"
#include "util/asset_path.h"
#include "util/subresource_access.h"

#include <filesystem>
#include <system_error>

namespace bro::bronze_host {

namespace ev = bronze::embed;
namespace fs = std::filesystem;

namespace {

// The layers below the bronze host (images, fonts, @import, <video>,
// <iframe>, scripts and sheets over http) ask the same one gate through
// util/subresource_access.h; this is the forwarder, put in place when the
// library loads: a file is a read, an http(s) URL a connection. Answered
// without asking: markup never raises a prompt.
std::string subresourceLoad(bool net, const std::string& subject) {
    return ev::checkAccessWithoutAsking(net ? ev::AccessKind::Net : ev::AccessKind::Read, subject);
}

[[maybe_unused]] const bool kSubresourceGateInstalled = [] {
    util::setSubresourceGate(&subresourceLoad);
    return true;
}();

}  // namespace

std::string accessPath(const std::string& path) {
    const fs::path p(std::u8string(path.begin(), path.end()));
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec) abs = p;
    const std::u8string u = abs.lexically_normal().u8string();
    return std::string(u.begin(), u.end());
}

std::string pathRefusal(const std::string& path, bool write, std::string* asked) {
    if (!ev::accessGateActive()) return {};
    std::string abs = accessPath(path);
    std::string refused = ev::checkAccess(write ? ev::AccessKind::Write : ev::AccessKind::Read, abs);
    if (asked) *asked = std::move(abs);
    return refused;
}

void requirePathAccess(const std::string& path, bool write) {
    std::string asked;
    const std::string refused = pathRefusal(path, write, &asked);
    if (!refused.empty()) ev::throwAccessError(refused, "EACCES", asked);
}

std::string readableAssetPath(const std::string& src) {
    std::string path = util::resolveAssetPath(src);
    requirePathAccess(path, false);
    return path;
}

void requireNetAccess(const std::string& url) {
    if (!ev::accessGateActive()) return;
    const std::string refused = ev::checkAccess(ev::AccessKind::Net, url);
    if (!refused.empty()) ev::throwAccessError(refused, "EACCES");
}

void requireSpawnAccess(const std::string& command) {
    if (!ev::accessGateActive()) return;
    const std::string refused = ev::checkAccess(ev::AccessKind::Spawn, command);
    if (!refused.empty()) ev::throwAccessError(refused, "EACCES");
}

}  // namespace bro::bronze_host
