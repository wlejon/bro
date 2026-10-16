#pragma once

// The agent inbox (docs/agent-inbox.md): agents running in a session
// register by name and take requests that other agents send them, through
// the agent control channel (control.h): a local `bro-ctl`, an app's
// `bro.inbox`, or another machine through broremote's text lane
// (`bro-ctl --remote HOST ask ...`). The session's inbox is the display's
// (every engine has one; agents and askers reach the display's by default).
//
// Everything is request/reply over the control protocol, so it crosses the
// remote unchanged:
//   - a serving agent registers (a name, a line saying what it does) and
//     long-polls `next`, which holds until a request or a cancel arrives or
//     the poll times out; it reports progress and replies by request id;
//   - an asker sends a request (text and files) and waits for the reply, or
//     takes the id and asks for its status (a long-poll that returns at the
//     next progress line) and its result later;
//   - every request carries who sent it: what the transport tells
//     (ControlOrigin: via, pid, user, ssh client, remote session name) and
//     what the sender declares (its agent name, its user@machine);
//   - an audit log on the host records requests, deliveries, progress,
//     replies, cancels and registrations, one JSON object per line.
//
// An agent is registered while it polls: its registration lapses kLeaseMs
// after its last poll ended, and what it held fails then. Authorization is
// the session, as for every control command; admits() is the seam where
// per-agent keys and capability lists go later.
//
// Engine thread only (every control command runs there); tick() runs from
// ControlServer::pump() to time out held polls. Times are the control
// clock's (virtual in headless, so advanceTime drives timeouts), stamps in
// replies and the log the wall clock's (unix ms).

#include "engine/control.h"
#include "platform/inbox_bundle.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bro::engine {

class Engine;

class AgentInbox {
public:
    using Clock = std::function<double()>;
    /// The answer to one inbox operation: ok, a JSON payload (or why it
    /// failed), and a files bundle (platform/inbox_bundle.h) for the
    /// operations that hand files back (next, result, ask), else empty.
    using Reply = std::function<void(bool ok, std::string payload, std::string bundle)>;

    /// How long a registration outlives its agent's last poll.
    static constexpr double kLeaseMs = 45000.0;
    /// The longest a `next` poll is held.
    static constexpr double kMaxPollMs = 25000.0;

    /// Who sent a request: what the transport says and what the sender declares.
    struct Sender {
        std::string agent;   // its self-declared agent name (bro-ctl: --from, else $BRO_AGENT_NAME, else user@machine)
        std::string origin;  // its self-declared user@machine
        ControlOrigin via;   // the transport's facts
    };

    AgentInbox(Clock clock, std::string logPath);
    ~AgentInbox();
    AgentInbox(const AgentInbox&) = delete;
    AgentInbox& operator=(const AgentInbox&) = delete;

    /// $BRO_INBOX_LOG, else <userDataDir>/agent-inbox.jsonl; headless, a file
    /// of its own in the temp directory (removed when the inbox goes).
    static std::string defaultLogPath(Engine& engine);
    const std::string& logPath() const { return logPath_; }

    // ---- an agent serving requests
    /// Registers `name` ([A-Za-z0-9._-], at most 64) and answers its token.
    /// A name another live agent holds is refused unless `replace`.
    void registerAgent(const std::string& name, const std::string& about, bool replace, const ControlOrigin& by,
                       const Reply& reply);
    /// The next request or cancel for the agent holding `token`, held up to
    /// `timeoutMs` (at most kMaxPollMs); {"type":"idle"} when none came.
    void next(const std::string& token, double timeoutMs, Reply reply);
    void progress(const std::string& token, const std::string& id, const std::string& text, const Reply& reply);
    /// The answer to request `id`: text and files, or (`failed`) why it failed.
    void answer(const std::string& token, const std::string& id, const std::string& text, const std::string& bundle,
                bool failed, const Reply& reply);
    void unregister(const std::string& token, const Reply& reply);

    // ---- asking
    /// The registered agents, as JSON (or as text, one per line).
    void agents(bool json, const Reply& reply);
    /// Sends a request. With `waitMs` > 0 the reply waits for the answer
    /// (its files in the bundle) up to that long, then answers the status;
    /// otherwise it answers the status (the id) at once.
    void ask(const std::string& to, const std::string& text, const std::string& bundle, const Sender& from,
             double waitMs, Reply reply);
    /// The request's state and its progress lines from index `since`; with
    /// `waitMs` > 0 held until there is a newer line or it finishes.
    void status(const std::string& id, size_t since, double waitMs, Reply reply);
    /// The answer (its files in the bundle), waiting up to `waitMs` for it.
    void result(const std::string& id, double waitMs, Reply reply);
    void cancel(const std::string& id, const Sender& by, const Reply& reply);
    /// What happened in the inbox since event `since` (a feed for a UI):
    /// {"next":N,"events":[...]}; with `waitMs` > 0 held until there is one.
    void events(uint64_t since, double waitMs, Reply reply);
    /// The audit log's last `limit` lines (JSON lines), those of request
    /// `id` only when it is given.
    void log(size_t limit, const std::string& id, const Reply& reply);

    /// Times out held polls and lapses registrations. Engine thread, often.
    void tick();

private:
    enum class State { Queued, Working, Done, Failed, Cancelled };
    struct Waiter {
        double deadline = 0;
        size_t since = 0;
        Reply reply;
    };
    struct Progress {
        double atMs = 0;  // since the request was asked
        std::string text;
    };
    struct Request {
        std::string id;
        std::string to;
        std::string text;
        std::vector<platform::inbox::File> files;
        Sender from;
        State state = State::Queued;
        double askedMs = 0, deliveredMs = 0, finishedMs = 0;
        int64_t askedUnix = 0;
        std::vector<Progress> progress;
        std::string answer;  // the reply's text (Done) or why it failed (Failed / Cancelled)
        std::vector<platform::inbox::File> answerFiles;
        std::vector<Waiter> resultWaiters;
        std::vector<Waiter> statusWaiters;
        bool finished() const { return state == State::Done || state == State::Failed || state == State::Cancelled; }
    };
    struct Agent {
        std::string name, about, token;
        ControlOrigin by;
        double registeredMs = 0, lastSeenMs = 0;
        int64_t registeredUnix = 0;
        uint64_t asked = 0, answered = 0;
        std::deque<std::string> queue;    // request ids not yet delivered
        std::deque<std::string> cancels;  // cancel notices for requests it holds
        std::unique_ptr<Waiter> poll;     // a `next` held
    };
    struct Event {
        uint64_t seq = 0;
        int64_t atUnix = 0;
        std::string kind, id, to, from, text, state;
    };

    Agent* byToken(const std::string& token);
    Agent* byName(const std::string& name);
    Request* find(const std::string& id);
    bool live(const Agent& a) const;
    /// The authorization seam (docs/agent-inbox.md, "Authorization"): the
    /// session is the credential today, so every request is admitted.
    bool admits(const Sender& from, const Agent& to, std::string* why) const;

    void deliver(Agent& a);
    void finish(Request& r, State state, std::string answer, std::vector<platform::inbox::File> files);
    void drop(const std::string& name, const std::string& why);
    void notifyStatus(Request& r);
    void prune();
    void emit(const std::string& kind, const Request* r, const std::string& text, const std::string& to = {},
              const std::string& from = {});
    void audit(const std::string& event, const Request* r, const std::string& text,
               const std::vector<platform::inbox::File>* files, const Agent* agent = nullptr,
               const ControlOrigin* by = nullptr);

    std::string statusJson(const Request& r, size_t since) const;
    std::string requestEventJson(const Request& r) const;
    static std::string senderJson(const Sender& s);
    static std::string originJson(const ControlOrigin& o);
    static std::string filesJson(const std::vector<platform::inbox::File>& files);
    static const char* stateName(State s);

    Clock clock_;
    std::string logPath_;
    std::map<std::string, Agent> agents_;
    std::map<std::string, std::unique_ptr<Request>> requests_;
    std::deque<std::string> finishedOrder_;  // finished request ids, oldest first (pruning)
    uint64_t nextRequest_ = 1;
    std::deque<Event> events_;
    uint64_t nextEvent_ = 1;
    std::vector<Waiter> eventWaiters_;
    std::string instance_;  // 4 hex digits: request ids stay apart across restarts in the log
};

}  // namespace bro::engine
