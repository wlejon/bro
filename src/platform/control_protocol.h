#pragma once

// The agent control protocol (docs/agent-control.md), shared by the engine's
// server (control_socket.cpp) and bro-ctl. Header-only over brolink's wire
// primitives, so bro-ctl links brolink and nothing of bro's.
//
// The transport is brolink's local IPC: endpoint <name> of application
// "bro-control" (brolink::local_address), a Unix socket in a private
// directory with the peer's uid checked, or on Windows a named pipe only the
// user can open. On it, brolink-framed messages (u32 length, u16 type, body):
//
//   Request  varint id, strings argv        the command line, argv[0] its name
//   Reply    varint id, bool ok, str payload text or JSON, the command's choice
//
// A connection may carry several requests; each reply names the request it
// answers and they come back in the order the commands finish. Nothing in it
// is specific to a local socket: any brolink stream (an ssh lane) can carry it.

#include <brolink/wire.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bro::platform::control {

/// brolink's application name: where the endpoints live.
inline constexpr std::string_view kApp = "bro-control";

enum : uint16_t {
    kRequest = 1,
    kReply = 2,
};

/// The largest message either side accepts.
inline constexpr size_t kMaxMessage = brolink::wire::kDefaultMaxMessage;

inline std::string encodeRequest(uint64_t id, const std::vector<std::string>& argv) {
    brolink::wire::Writer w;
    w.varint(id);
    w.strings(argv);
    return brolink::wire::make_message(kRequest, w.data());
}

inline bool decodeRequest(std::string_view body, uint64_t& id, std::vector<std::string>& argv) {
    brolink::wire::Reader r(body);
    id = r.varint();
    argv = r.strings();
    return r.done();
}

inline std::string encodeReply(uint64_t id, bool ok, std::string_view payload) {
    brolink::wire::Writer w;
    w.varint(id);
    w.boolean(ok);
    w.str(payload);
    return brolink::wire::make_message(kReply, w.data());
}

inline bool decodeReply(std::string_view body, uint64_t& id, bool& ok, std::string& payload) {
    brolink::wire::Reader r(body);
    id = r.varint();
    ok = r.boolean();
    payload = r.str();
    return r.done();
}

}  // namespace bro::platform::control
