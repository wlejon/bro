#pragma once
// Persistent terminal sessions: shells owned by a per-user bromux server, so
// they outlive the <terminal> that shows them (element removal, a page
// reload, bro itself). TermSession::spawnPersistent / attach (term_session.h)
// connect an element to one; this is the rest of the surface -- what the
// server holds, and closing it.
//
// A server is named (empty: bromux's default, the per-user server the
// `bromux` command line uses too). The client starts it on demand from the
// `bromux` executable built beside bro; the calls here never start one, so
// listing sessions with no server running answers an empty list.
//
// Built only with bromux (BRO_TERMINAL_MUX); without it every call fails
// with "persistent sessions are not built in".

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bro::terminal {

struct MuxSessionInfo {
    uint64_t id = 0;
    std::string name;      // the "name" meta key
    std::string command;
    int64_t pid = 0;
    bool running = false;
    int exitCode = -1;     // once not running; -1 unknown
    int cols = 0, rows = 0;
    uint32_t clients = 0;  // attached clients (elements, bromux attach, ...)
    uint64_t createdMs = 0;
    std::string title;
    std::string cwd;
};

[[nodiscard]] bool muxAvailable();
std::optional<std::vector<MuxSessionInfo>> muxListSessions(const std::string& server, std::string* error);
// Close a session: its program is killed and the session removed.
bool muxCloseSession(const std::string& server, uint64_t id, std::string* error);
// Stop the server and every session in it.
bool muxKillServer(const std::string& server, std::string* error);

} // namespace bro::terminal
