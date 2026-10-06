// Persistent sessions: TermSessions over a bromux server (term_session.h).
// A shell created through one TermSession survives that TermSession's
// destruction; a new one attaches by id and shows the same screen and the
// same history; two attached at once share input and output; the program's
// exit reaches both; kill() closes the session. A private server (named for
// this process) is started for the test and stopped at the end.

#include "check.h"
#include "tests.h"

#include "terminal/term_mux.h"
#include "terminal/term_session.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace bro::terminal::test {

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds limit = 15s) {
    const auto until = Clock::now() + limit;
    while (Clock::now() < until) {
        if (pred()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

std::string serverName() {
#if defined(_WIN32)
    return "bro-test-" + std::to_string(_getpid());
#else
    return "bro-test-" + std::to_string(getpid());
#endif
}

#if defined(_WIN32)
const char* const kEnter = "\r";
// Sixty numbered lines: more than the 10-row screen holds, so some are history.
const char* const kLines = "for /L %i in (1,1,60) do @echo line-%i";
SpawnOptions shellOptions() {
    SpawnOptions o;
    o.command = "cmd.exe";
    o.args = {"/d"};
    return o;
}
#else
const char* const kEnter = "\n";
const char* const kLines = "i=1; while [ $i -le 60 ]; do echo line-$i; i=$((i+1)); done";
SpawnOptions shellOptions() {
    SpawnOptions o;
    o.command = "/bin/sh";
    o.env = {{"PS1", "$ "}};
    return o;
}
#endif

std::optional<MuxSessionInfo> findSession(const std::string& server, uint64_t id) {
    std::string err;
    auto list = muxListSessions(server, &err);
    if (!list) return std::nullopt;
    for (const MuxSessionInfo& s : *list)
        if (s.id == id) return s;
    return std::nullopt;
}

bool contains(const std::string& s, const std::string& needle) { return s.find(needle) != std::string::npos; }

} // namespace

void run_session_mux_tests() {
    if (!muxAvailable()) {
        std::printf("[session mux: skipped, built without bromux]\n");
        return;
    }
    const std::string server = serverName();
    std::string err;

    std::printf("[session mux: a persistent shell outlives its TermSession]\n");
    uint64_t id = 0;
    std::string screen, history;
    {
        TermSession a(80, 10);
        TermSession::PersistentOptions p;
        p.server = server;
        p.name = "mux-test";
        const bool ok = a.spawnPersistent(shellOptions(), p, &err);
        CHECK_MSG(ok, "spawnPersistent: " + err);
        if (!ok) return;
        id = a.sessionId();
        CHECK(id != 0);
        CHECK(a.persistent() && a.running());
        CHECK(a.pid() > 0);
        a.resize(80, 10, 8, 16);
        a.write(std::string(kLines) + kEnter);
        CHECK_MSG(waitFor([&] { return contains(a.screenText(), "line-60"); }), "output: " + a.screenText());
        // The screen settles (the prompt after the loop).
        std::this_thread::sleep_for(300ms);
        screen = a.screenText();
        history = a.scrollbackText();
        CHECK_MSG(contains(history, "line-1\n") || contains(history, "line-1"), "history: " + history);
        auto info = findSession(server, id);
        CHECK(info && info->name == "mux-test" && info->running && info->clients == 1);
    }  // destroyed: detaches, the shell runs on

    std::printf("[session mux: reattach by id shows the same screen and history]\n");
    {
        auto info = findSession(server, id);
        CHECK_MSG(info && info->running, "the session is still there and running after its TermSession went");
        TermSession b(80, 10);
        const bool ok = b.attach(id, server, &err);
        CHECK_MSG(ok, "attach: " + err);
        if (!ok) return;
        CHECK(b.sessionId() == id && b.running());
        CHECK_MSG(waitFor([&] { return b.screenText() == screen; }),
                  "reattached screen:\n" + b.screenText() + "\n-- was --\n" + screen);
        CHECK_MSG(b.scrollbackText() == history, "reattached history differs");
        // It is live: input reaches it.
        b.write(std::string("echo again-") + "ok" + kEnter);
        CHECK(waitFor([&] { return contains(b.screenText(), "again-ok"); }));
        // The element's theme shows through what the program left alone.
        bropty::Palette theme = bropty::Palette::standard();
        theme.foreground = bropty::Rgb{1, 2, 3};
        theme.colors[1] = bropty::Rgb{4, 5, 6};
        b.setBasePalette(theme);
        CHECK(waitFor([&] {
            auto f = b.acquireFrame();
            return f && f->palette && f->palette->foreground == bropty::Rgb{1, 2, 3} &&
                   f->palette->colors[1] == bropty::Rgb{4, 5, 6};
        }));
        CHECK(b.palette().foreground == (bropty::Rgb{1, 2, 3}));
        // detach() lets go explicitly; the shell runs on.
        b.detach();
        CHECK(b.detached() && !b.running());
        CHECK(!b.write("x"));
        CHECK(waitFor([&] {
            auto i = findSession(server, id);
            return i && i->running && i->clients == 0;
        }));
    }

    std::printf("[session mux: two TermSessions on one session; exit reaches both]\n");
    {
        TermSession c(80, 10), d(80, 10);
        CHECK(c.attach(id, server, &err));
        CHECK(d.attach(id, server, &err));
        c.write(std::string("echo from-c") + kEnter);
        CHECK(waitFor([&] { return contains(d.screenText(), "from-c"); }));
        d.sendText("echo from-d");
        d.write(kEnter);
        CHECK(waitFor([&] { return contains(c.screenText(), "from-d"); }));
        c.write(std::string("exit 7") + kEnter);
        CHECK(waitFor([&] { return c.exited() && d.exited(); }));
        CHECK(c.exitCode() && *c.exitCode() == 7);
        CHECK(d.exitCode() && *d.exitCode() == 7);
        CHECK(!c.running() && !d.running());
        auto info = findSession(server, id);
        CHECK(info && !info->running && info->exitCode == 7);
    }

    std::printf("[session mux: kill() closes the session]\n");
    {
        TermSession e(80, 10);
        TermSession::PersistentOptions p;
        p.server = server;
        CHECK(e.spawnPersistent(shellOptions(), p, &err));
        const uint64_t eid = e.sessionId();
        CHECK(findSession(server, eid).has_value());
        e.kill();
        CHECK(waitFor([&] { return e.exited(); }));
        CHECK(waitFor([&] { return !findSession(server, eid).has_value(); }));
        CHECK(muxCloseSession(server, id, &err));
        CHECK(!findSession(server, id).has_value());
    }

    CHECK_MSG(muxKillServer(server, &err), "kill server: " + err);
    auto none = muxListSessions(server, &err);
    CHECK(none && none->empty());
}

} // namespace bro::terminal::test
