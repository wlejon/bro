#pragma once

// The desktop shell's permission prompts (docs/apps.md, Asking): a local
// channel over brolink on which a shell shows the user an app's request for a
// capability, and the app waits for the answer.
//
// The endpoint is brolink application "bro-prompt", one per user (POSIX: a
// 0600 socket in the user's private runtime directory; Windows: a pipe whose
// DACL admits only the user, per logon session). Its name is the session's
// (endpointName below); a scratch profile (BRO_APP_HOME) has a name of its
// own, derived from the profile's folder, so a test never reaches the user's
// real shell. Both ends check the other is the same user.
//
// One connection per question: the app connects, sends kAsk, and reads one
// kAnswer; a connection closed without one (the shell dropped the request,
// stopped listening, or went away) is no answer. The shell's answer is only
// allow or deny: the asking process keeps the decision for its own identity,
// so a process that lies about who it is can only ever grant itself.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bro::platform::prompts {

/// What an app says about itself and what it asks for. The app fields are
/// the requester's claims; the shell judges the capability by its own name.
struct Question {
    std::string appName, appId, identity, dir;
    std::string capability;
    std::string what;   // what it was trying to do ("start git"), may be ""
};

/// The endpoint name for this session, or for the scratch profile at
/// `scratchHome` (engine::scratchProfileHome(); "" for the user's own).
std::string endpointName(const std::string& scratchHome);

enum class AskResult {
    NoShell,    // nobody listens there (or this process is the one serving): ask some other way
    Allow,
    Deny,
    NoAnswer,   // the shell dropped it, went away, or this process is quitting
};

/// Ask the shell serving `endpoint` and wait for its answer on the calling
/// (main) thread, keeping the window system's events pumped meanwhile, as a
/// modal dialog does. Ends early (NoAnswer) when the process is interrupted.
AskResult ask(const std::string& endpoint, const Question& q);

/// The shell's end. A thread runs brolink's event loop; requests and
/// cancellations are collected for take() on the main thread, which answers
/// through respond().
class Server {
public:
    struct Event {
        enum class Kind { Request, Cancel } kind = Kind::Request;
        uint64_t id = 0;        // the connection: one question each
        uint64_t peerPid = 0;   // the process that asked (brolink, at connect)
        Question question;      // Request only
    };

    Server();
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Serve `endpoint`. False (`why` says why) when another live server
    /// owns it, or it cannot be bound.
    bool start(const std::string& endpoint, std::string* why = nullptr);
    /// Stop serving: every outstanding request's connection closes (no answer).
    void stop();
    bool running() const;

    /// Requests and cancellations since the last call, in order. A request
    /// answered or dropped is never cancelled afterwards.
    std::vector<Event> take();
    /// Answer request `id` (allow or deny) and close it; false when it is
    /// gone (cancelled) or was answered already.
    bool respond(uint64_t id, bool allow);
    /// Drop request `id` with no answer (the asker refuses, keeps nothing).
    bool drop(uint64_t id);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Whether this process serves prompts now: it then never asks itself.
bool servingInProcess();

}  // namespace bro::platform::prompts
