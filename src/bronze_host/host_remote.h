#pragma once
// bro.remote's host side (BRO_WITH_REMOTE): what broremote's JavaScript
// binding (broremote_api) reaches through its HostHooks. While a server is
// hosted it is fed every composited frame — under DRM the KMS scanout
// buffer itself, otherwise a CPU readback — and its viewers' input goes into
// the engine's own input path; with no server it costs one branch a frame.
// See host_remote.cpp.

namespace bro::engine {
class Engine;
}

namespace bro::bronze_host {

// Mounts bro.remote on this realm, and the first time also sets the hooks,
// the frame pump and the shutdown hook. installSiblingApis is the only
// caller (one call per realm).
void installRemoteHost(engine::Engine& engine);

}  // namespace bro::bronze_host
