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
#include <mutex>
#include <string>
#include <vector>

namespace bro::platform { class ControlSocket; }

namespace bro::engine {

class Engine;
class AgentInbox;

/// Where a command came from, as the transport tells it (not as the client
/// says): what the agent inbox stamps on a request and logs. Facts to
/// report, not a credential: authorization is reaching the endpoint at all.
struct ControlOrigin {
    /// "local": this user's control socket (bro-ctl, an app's bro.inbox);
    /// "remote": a broremote session's text lane (bro-ctl --remote, a
    /// viewer's session.request()); "app": in-process (this page's JS, a
    /// headless test). Empty: not known.
    std::string via;
    uint64_t pid = 0;       // the process at the other end (the remote: its `broremote proxy`); 0 unknown
    std::string user;       // the login it runs as (the host's own user, always)
    std::string sshClient;  // the ssh client address whose login started that process; empty: none
    std::string client;     // remote: the session's name (its Hello)
};

/// One invocation of a command. Reply exactly once — now, or on a later
/// frame from a ticker that holds the shared_ptr.
class ControlCall {
public:
    ControlCall(std::vector<std::string> argv, std::function<void(bool, std::string)> sink, ControlOrigin origin = {})
        : argv_(std::move(argv)), sink_(std::move(sink)), origin_(std::move(origin)) {}

    /// argv[0] is the command name.
    const std::vector<std::string>& argv() const { return argv_; }
    const ControlOrigin& origin() const { return origin_; }

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
    ControlOrigin origin_;
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
    /// A registered command's handler and usage (to wrap it); a null handler when there is none.
    Handler handler(const std::string& name, std::string* usage = nullptr) const;
    void addTicker(Ticker ticker) { tickers_.push_back(std::move(ticker)); }
    /// Whether a ticker is running: every frame is then needed.
    bool hasTickers() const { return !tickers_.empty(); }

    /// Engine thread, between frames: runs the commands that arrived and
    /// this frame's tickers.
    void pump();

    /// Runs one command line in-process (tests, and a command built from
    /// others); the reply arrives through `sink`, possibly frames later.
    /// `origin` defaults to in-process ("app").
    void dispatch(std::vector<std::string> argv, std::function<void(bool, std::string)> sink,
                  ControlOrigin origin = inProcess());
    static ControlOrigin inProcess();

    /// A reply with the bytes of the command's output file (empty when none).
    using FileSink = std::function<void(bool ok, std::string payload, std::string file)>;
    /// dispatch(), where argv[outArg] (when outArg is nonzero) names an
    /// output file the client cannot read where the engine writes it: the
    /// command writes a private file of the engine's instead (same
    /// extension), whose bytes come back to `sink` (then it is removed), and
    /// the reply text names the client's argument rather than that file.
    /// Engine thread. Every command works this way with no code of its own.
    void dispatchWithFile(std::vector<std::string> argv, size_t outArg, FileSink sink,
                          ControlOrigin origin = inProcess());
    /// Any thread: dispatchWithFile on the engine thread, at the next pump()
    /// (which it wakes the frame loop for). `sink` is called once, from
    /// whatever thread finishes the command. How a remote session's text
    /// lane (broremote) reaches the same commands a local bro-ctl does.
    void post(std::vector<std::string> argv, size_t outArg, FileSink sink, ControlOrigin origin);

    /// The agent inbox (agent_inbox.h, docs/agent-inbox.md): agents of the
    /// session registered by name, and the requests other agents send them.
    /// Every engine has one; the session's is the display's.
    AgentInbox& inbox() { return *inbox_; }

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
    struct Posted {
        std::vector<std::string> argv;
        size_t outArg = 0;
        FileSink sink;
        ControlOrigin origin;
    };
    Engine& engine_;
    std::shared_ptr<platform::ControlSocket> socket_;
    std::unique_ptr<AgentInbox> inbox_;
    std::map<std::string, Command> commands_;
    std::vector<Ticker> tickers_;
    std::mutex postedMutex_;
    std::vector<Posted> posted_;  // under postedMutex_
};

/// The engine's built-in commands (control_commands.cpp and friends).
void registerEngineControlCommands(ControlServer& server);
void registerControlInputCommands(ControlServer& server);
void registerControlRecordCommands(ControlServer& server);
/// The agent view (control_view.cpp, docs/agent-view.md): view, windows,
/// shot, the handle forms of the input commands. Registered after the input
/// commands, which it wraps.
void registerAgentViewCommands(ControlServer& server);
void releaseControlView(const ControlServer* server);
/// The agent inbox's commands (control_inbox.cpp): agents, ask, inbox.
void registerInboxCommands(ControlServer& server);
/// Whether this user's session has a display serving agent control (the
/// "display" endpoint exists): an app then serves its own, for the display
/// to find by process id, however it was started.
bool controlDisplayServes();

}  // namespace bro::engine
