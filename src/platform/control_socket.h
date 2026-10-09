#pragma once

// The listening half of the agent control channel (docs/agent-control.md),
// on brolink's local IPC: endpoint <name> of brolink application
// "bro-control" — a Unix socket in a 0700 directory, 0600, the peer's uid
// checked on every connection; on Windows a named pipe whose DACL admits only
// the user and refuses remote clients. Never a network listener. brolink
// also owns the endpoint's lifetime: a live server keeps its name, a dead
// one's is reclaimed, and binding sweeps every dead server's socket in the
// directory.
//
// The messages are control_protocol.h's. A thread here runs brolink's event
// loop: it accepts, splits the frames, and hands requests to whoever polls
// take() (the engine, between frames); replies go back through reply() from
// any thread.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bro::platform {

class ControlSocket {
public:
    struct Request {
        uint64_t conn = 0;  // which connection to reply on
        uint64_t id = 0;    // the request's id, echoed in its reply
        std::vector<std::string> argv;
        std::string parseError;  // set when the message was not a request
    };

    ControlSocket();
    ~ControlSocket();
    ControlSocket(const ControlSocket&) = delete;
    ControlSocket& operator=(const ControlSocket&) = delete;

    /// Serves endpoint `name` and starts the thread. A live server already
    /// on the name keeps it (false, `why` says so).
    bool start(const std::string& name, std::string* why = nullptr);
    void stop();
    bool running() const;
    /// The address served (a socket path, or a pipe name on Windows).
    const std::string& path() const;

    /// Requests received since the last call.
    std::vector<Request> take();
    /// Readable while requests wait for take(), for a caller that sleeps in
    /// poll() and should wake for a command (-1 when not running, and on
    /// Windows).
    int readyFd() const;
    /// Queues the reply to request `id` on `conn` (dropped if it has gone).
    /// Any thread.
    void reply(uint64_t conn, uint64_t id, bool ok, std::string payload);

    /// A private per-user directory for the files commands write for their
    /// client (bro-ctl -o -): the socket directory on POSIX.
    static std::string runtimeDir();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bro::platform
