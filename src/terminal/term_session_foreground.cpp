// TermSession's foreground process: bropty answers what owns the terminal
// (IPtyProcess::foreground_process); this decides when to ask. Asking costs
// a few system calls on POSIX and a process snapshot on Windows, so it is
// done on the parser thread, shortly after the input or output that usually
// accompanies a change, and rarely otherwise.
#include "terminal/term_session.h"

#include <algorithm>

namespace bro::terminal {

namespace {

using Clock = std::chrono::steady_clock;

// A program started by a keystroke is running a moment later, not at once.
constexpr std::chrono::milliseconds kSettle{60};
// After activity, checks at these delays catch a change that came later
// with nothing printed (a program that starts silently).
constexpr std::chrono::milliseconds kTrail[] = {std::chrono::milliseconds(500), std::chrono::milliseconds(1500)};

} // namespace

std::optional<bropty::ProcessInfo> TermSession::foregroundProcess() const {
    std::lock_guard<std::mutex> g(fgMu_);
    return fg_;
}

Clock::time_point TermSession::pollForeground(Clock::time_point now, bool activity) {
    if (fgFinal_ || !spawned()) return {};
    if (activity) {
        fgTrail_ = int(std::size(kTrail));
        const Clock::time_point due = std::max(now + kSettle, fgLast_ + kForegroundGap);
        if (fgDue_ == Clock::time_point{} || due < fgDue_) fgDue_ = due;
    }
    if (fgDue_ == Clock::time_point{}) fgDue_ = fgLast_ + kForegroundIdle;
    const bool done = exited();
    if (!done && now < fgDue_) return fgDue_;

    std::shared_ptr<bropty::IPtyProcess> pty;
    {
        std::lock_guard<std::mutex> g(mu_);
        pty = pty_;
    }
    // Outside mu_: input is not held up behind a process snapshot.
    std::optional<bropty::ProcessInfo> info;
    if (pty && !done) info = pty->foreground_process();
    fgLast_ = now;
    fgDue_ = {};
    if (fgTrail_ > 0) {
        fgDue_ = now + kTrail[std::size(kTrail) - size_t(fgTrail_)];
        --fgTrail_;
    }
    bool changed;
    {
        std::lock_guard<std::mutex> g(fgMu_);
        changed = info != fg_;
        if (changed) fg_ = std::move(info);
    }
    if (changed) {
        TermEvent e;
        e.kind = TermEvent::Kind::Foreground;
        pushEvent(std::move(e));
    }
    if (done) {
        fgFinal_ = true;
        return {};
    }
    return fgDue_ != Clock::time_point{} ? fgDue_ : now + kForegroundIdle;
}

} // namespace bro::terminal
