#pragma once

// The agent control channel (docs/agent-control.md): a local, same-user
// socket through which a tool — bro-ctl, and through it an AI agent over
// ssh — drives and observes a running bro: screenshots and recordings,
// scripted input, JS evaluated in the page's realm, the DOM, the running
// animations and the frame flight recorder.
//
// The socket (platform::ControlSocket) is read on its own thread; every
// command runs on the engine thread, between frames (pump(), called by the
// frame loop), where the DOM and the layout boxes are the main thread's to
// read. A command replies at once or keeps its call and replies on a later
// frame (input that plays out over time, a recording, waiting for idle):
// tickers run once per frame until they say they are done.
//
// Commands register by name. The engine's own are in control_commands.cpp,
// control_input.cpp and control_record.cpp; bronze_host adds the ones that
// need the JS realm (eval, dom, inspect) in host_control.cpp.

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bro::platform { class ControlSocket; }

namespace bro::engine {

class Engine;

/// One invocation of a command. Reply exactly once — now, or on a later
/// frame from a ticker that holds the shared_ptr.
class ControlCall {
public:
    ControlCall(std::vector<std::string> argv, std::function<void(bool, std::string)> sink)
        : argv_(std::move(argv)), sink_(std::move(sink)) {}

    /// argv[0] is the command name.
    const std::vector<std::string>& argv() const { return argv_; }

    /// Arguments that are not --options, after the command name.
    std::vector<std::string> positional() const;
    std::string arg(size_t i, const std::string& fallback = {}) const;
    /// --name=value (or --name alone, which reads as "true"); fallback when absent.
    std::string option(const std::string& name, const std::string& fallback = {}) const;
    bool flag(const std::string& name) const;
    double number(const std::string& name, double fallback) const;

    void ok(std::string payload = {}) { finish(true, std::move(payload)); }
    void fail(std::string message) { finish(false, std::move(message)); }
    bool done() const { return done_; }

private:
    void finish(bool ok, std::string payload) {
        if (done_) return;
        done_ = true;
        if (sink_) sink_(ok, std::move(payload));
    }
    std::vector<std::string> argv_;
    std::function<void(bool, std::string)> sink_;
    bool done_ = false;
};

using ControlCallPtr = std::shared_ptr<ControlCall>;

class ControlServer {
public:
    using Handler = std::function<void(const ControlCallPtr&)>;
    /// Runs once a frame; true when finished (it is then dropped).
    using Ticker = std::function<bool()>;

    explicit ControlServer(Engine& engine);
    ~ControlServer();

    /// Starts serving control endpoint <name> (brolink's local IPC:
    /// $XDG_RUNTIME_DIR/bro-control/<name>.sock, a named pipe on Windows).
    bool start(const std::string& name, std::string* why = nullptr);
    void stop();
    bool running() const;
    std::string socketPath() const;
    /// Readable while commands wait for pump(): the frame loop's pacing
    /// sleep wakes on it (-1 when not serving).
    int pollFd() const;

    /// `usage` is one line: the arguments, then what it does.
    void registerCommand(const std::string& name, const std::string& usage, Handler handler);
    void addTicker(Ticker ticker) { tickers_.push_back(std::move(ticker)); }

    /// Engine thread, between frames: runs the commands that arrived and
    /// this frame's tickers.
    void pump();

    /// Runs one command line in-process (tests, and a command built from
    /// others); the reply arrives through `sink`, possibly frames later.
    void dispatch(std::vector<std::string> argv, std::function<void(bool, std::string)> sink);

    std::string helpText() const;
    Engine& engine() { return engine_; }
    /// The clock input timelines play out on: the wall clock, or headless
    /// the virtual one advanceTime moves.
    double clockMs() const;

private:
    struct Command {
        std::string usage;
        Handler handler;
    };
    Engine& engine_;
    std::unique_ptr<platform::ControlSocket> socket_;
    std::map<std::string, Command> commands_;
    std::vector<Ticker> tickers_;
};

/// The engine's built-in commands (control_commands.cpp and friends).
void registerEngineControlCommands(ControlServer& server);
void registerControlInputCommands(ControlServer& server);
void registerControlRecordCommands(ControlServer& server);

}  // namespace bro::engine
