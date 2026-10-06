// The bromux calls that need no element: list, close, kill (term_mux.h), and
// the connection options every bro client uses (term_mux_connect.h).
#include "terminal/term_mux.h"
#include "terminal/term_mux_connect.h"

#include <bromux/client.h>
#include <bromux/paths.h>

namespace bro::terminal {

bromux::ConnectOptions muxConnectOptions(const std::string& server, bool autostart) {
    bromux::ConnectOptions o;
    o.server_name = server;
    o.autostart = autostart;
    o.client_name = "bro";
    o.timeout = std::chrono::milliseconds(10000);
    return o;
}

bool muxAvailable() { return true; }

namespace {

// A connection to a running server, never starting one. Null (with *error)
// when there is none.
std::unique_ptr<bromux::Client> connectRunning(const std::string& server, std::string* error) {
    if (!server.empty() && !bromux::valid_server_name(server)) {
        if (error) *error = "invalid server name (1-64 of A-Z a-z 0-9 _ . -)";
        return nullptr;
    }
    return bromux::Client::connect(muxConnectOptions(server, /*autostart=*/false), error);
}

} // namespace

std::optional<std::vector<MuxSessionInfo>> muxListSessions(const std::string& server, std::string* error) {
    std::string why;
    std::unique_ptr<bromux::Client> c = connectRunning(server, &why);
    if (!c) {
        if (!server.empty() && !bromux::valid_server_name(server)) {
            if (error) *error = why;
            return std::nullopt;
        }
        return std::vector<MuxSessionInfo>{};  // no server: no sessions
    }
    std::optional<std::vector<bromux::SessionInfo>> list = c->list_sessions(error);
    if (!list) return std::nullopt;
    std::vector<MuxSessionInfo> out;
    out.reserve(list->size());
    for (const bromux::SessionInfo& s : *list) {
        MuxSessionInfo m;
        m.id = s.id;
        m.name = s.meta_value("name");
        m.command = s.command;
        m.pid = s.pid;
        m.running = s.running;
        m.exitCode = s.exit_code;
        m.cols = s.cols;
        m.rows = s.rows;
        m.clients = s.clients;
        m.createdMs = s.created_ms;
        m.title = s.title;
        m.cwd = s.cwd;
        out.push_back(std::move(m));
    }
    return out;
}

bool muxCloseSession(const std::string& server, uint64_t id, std::string* error) {
    std::unique_ptr<bromux::Client> c = connectRunning(server, error);
    return c && c->close_session(id, error);
}

bool muxKillServer(const std::string& server, std::string* error) {
    std::unique_ptr<bromux::Client> c = connectRunning(server, error);
    if (!c) return false;
    c->kill_server();
    // The server closes every connection as it goes: wait for ours.
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::vector<bromux::ClientEvent> events;
    while (c->connected() && std::chrono::steady_clock::now() < end) {
        c->wait(std::chrono::milliseconds(100));
        c->dispatch(events);
        events.clear();
    }
    return !c->connected();
}

} // namespace bro::terminal
