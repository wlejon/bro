#pragma once

// The listening half of the agent control channel (docs/agent-control.md):
// a Unix socket under $XDG_RUNTIME_DIR/bro-control/ that only the user who
// owns the process can reach (a 0700 directory, a 0600 socket, and the
// peer's uid checked on every connection). Never a network listener.
//
// Wire format, both directions line-framed:
//   request   one line: a JSON array of strings, the command's argv
//             ["click", "#launcher"]
//   reply     a header line "ok <bytes>" or "error <bytes>", then exactly
//             that many bytes of payload (text or JSON, the command's choice)
// A connection may carry several requests; replies come back in the order
// the commands finish. A thread here accepts and reads; requests are handed
// to whoever polls take() (the engine, between frames), and replies go back
// through reply() from any thread.
//
// On Windows (10 1803 and later) it is the same AF_UNIX socket through
// Winsock, under %TEMP%\bro-control\ (the user's own directory) when
// $XDG_RUNTIME_DIR is unset; Winsock has no peer credentials, so that
// directory's ACL is the gate. readyFd() is -1 there.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bro::platform {

class ControlSocket {
public:
    struct Request {
        uint64_t conn = 0;  // which connection to reply on
        std::vector<std::string> argv;
        std::string parseError;  // set when the line was not a JSON string array
    };

    ControlSocket();
    ~ControlSocket();
    ControlSocket(const ControlSocket&) = delete;
    ControlSocket& operator=(const ControlSocket&) = delete;

    /// Binds $XDG_RUNTIME_DIR/bro-control/<name>.sock and starts the thread.
    /// A stale socket (nothing answering) is replaced; a live one is not.
    bool start(const std::string& name, std::string* why = nullptr);
    void stop();
    bool running() const;
    const std::string& path() const;

    /// Requests received since the last call.
    std::vector<Request> take();
    /// Readable while requests wait for take(), for a caller that sleeps in
    /// poll() and should wake for a command (-1 when not running).
    int readyFd() const;
    /// Queues a reply on `conn` (dropped if it has gone). Any thread.
    void reply(uint64_t conn, bool ok, std::string payload);

    /// The directory the sockets live in.
    static std::string socketDir();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Parses a JSON array of strings (the request line). Numbers, booleans and
/// null inside it are taken as their text. False, with `why`, on anything else.
bool parseJsonStringArray(const std::string& line, std::vector<std::string>& out, std::string* why);

}  // namespace bro::platform
