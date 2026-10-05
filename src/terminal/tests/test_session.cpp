// TermSession: the parser thread between a real PTY child (bropty's
// pty_child) and the frames the main thread paints. Output arrives, input
// reaches the child byte for byte, a resize reaches its window size, exit
// codes come back, a flood neither starves control calls nor loses a byte,
// and synchronized output holds presentation until the update ends.

#include "check.h"
#include "tests.h"

#include "terminal/term_session.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

namespace bro::terminal::test {

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::string childPath() { return (std::filesystem::path(g_exeDir) / BRO_PTY_CHILD_NAME).string(); }

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds limit = 10s) {
    const auto until = Clock::now() + limit;
    while (Clock::now() < until) {
        if (pred()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

std::string frameText(const bropty::Frame& f) {
    std::string out;
    for (size_t y = 0; y < f.lines.size(); ++y) {
        if (y) out.push_back('\n');
        if (f.lines[y]) out += f.lines[y]->view().text();
    }
    return out;
}

// The newest frame's text (the main thread's view of the screen).
std::string presented(TermSession& s) {
    auto f = s.acquireFrame();
    return f ? frameText(*f) : std::string();
}

bool spawnChild(TermSession& s, std::vector<std::string> args) {
    SpawnOptions o;
    o.command = childPath();
    o.args = std::move(args);
    std::string err;
    const bool ok = s.spawn(o, &err);
    CHECK_MSG(ok, "spawn " + o.command + ": " + err);
    return ok;
}

bool contains(const std::string& hay, std::string_view needle) { return hay.find(needle) != std::string::npos; }

void testFeed() {
    section("session: feed, screen text, frames");
    TermSession s(40, 6);
    s.feed("hello\r\n\x1b[31mworld\x1b[0m");
    CHECK(s.screenText() == "hello\nworld");
    CHECK(waitFor([&] { return contains(presented(s), "world"); }));
    CHECK(s.cursor().row == 1 && s.cursor().col == 5);
    CHECK(!s.spawned() && !s.running());
    CHECK(!s.write("x"));  // no child
}

void testPrintAndExit() {
    section("session: child output and exit code");
    {
        TermSession s(40, 6);
        if (!spawnChild(s, {"print", "first\\nsecond \\e[1mbold\\e[0m"})) return;
        CHECK(s.pid() > 0);
        CHECK(waitFor([&] { return s.exited(); }));
        CHECK(s.exitCode() == 0);
        CHECK_MSG(contains(s.screenText(), "first\nsecond bold"), s.screenText());
        CHECK(waitFor([&] { return contains(presented(s), "second bold"); }));
    }
    {
        TermSession s(40, 6);
        if (!spawnChild(s, {"exit", "7"})) return;
        CHECK(waitFor([&] { return s.exited(); }));
        CHECK(s.exitCode() == 7);
    }
    {
        TermSession s(40, 6);
        if (!spawnChild(s, {"stubborn"})) return;
        CHECK(waitFor([&] { return contains(s.screenText(), "READY"); }));
        s.kill();
        CHECK(waitFor([&] { return s.exited(); }, 15s));
    }
}

void testInput() {
    section("session: input reaches the child");
    TermSession s(40, 6);
    if (!spawnChild(s, {"keys", "7"})) return;
    CHECK(waitFor([&] { return contains(s.screenText(), "READY"); }));
    CHECK(s.sendText("ab"));
    bropty::KeyEvent enter;
    enter.key = bropty::Key::Enter;
    CHECK(s.sendKey(enter));
    bropty::KeyEvent ctrlC;
    ctrlC.codepoint = U'c';
    ctrlC.mods = bropty::Mod_Ctrl;
    CHECK(s.sendKey(ctrlC));
    CHECK(s.write("\xc3\xa9z"));  // UTF-8 straight through
    CHECK(waitFor([&] { return s.exited(); }));
    // a b CR ^C e-acute z
    CHECK_MSG(contains(s.screenText(), "KEYS:6162" "0d" "03" "c3a97a"), s.screenText());
}

void testResize() {
    section("session: resize reaches the child");
    TermSession s(40, 6);
    if (!spawnChild(s, {"size"})) return;
    CHECK_MSG(waitFor([&] { return contains(s.screenText(), "SIZE 6x40"); }), s.screenText());
    CHECK(s.resize(50, 10, 8, 16));
    CHECK(!s.resize(50, 10, 8, 16));  // unchanged
    CHECK(s.cols() == 50 && s.rows() == 10);
    s.write("x");
    CHECK_MSG(waitFor([&] { return contains(s.screenText(), "SIZE 10x50"); }), s.screenText());
    s.write("q");
    CHECK(waitFor([&] { return s.exited(); }));
}

void testFlood() {
    section("session: flood");
    constexpr long long kBytes = 32ll << 20;
    {
        // The emulator alone, no PTY: the same bytes fed in 64 KB chunks.
        TermSession p(100, 30);
        std::string chunk;
        long long made = 0;
        for (long long n = 0; chunk.size() < (64u << 10); ++n)
            chunk += "flood line " + std::to_string(n) + " ................................................\r\n";
        const auto a = Clock::now();
        for (made = 0; made < kBytes; made += (long long)chunk.size()) p.feed(chunk);
        const double secs = std::chrono::duration<double>(Clock::now() - a).count();
        std::printf("  parse only: %.1f MB in %.2f s = %.1f MB/s\n", double(made) / (1 << 20), secs,
                    double(made) / (1 << 20) / secs);
    }
    {
        // The PTY alone, no emulator: the ceiling the platform's PTY and the
        // child put on any terminal.
        auto pty = bropty::create_pty();
        bropty::PtyConfig cfg;
        cfg.command = childPath();
        cfg.args = {"flood", std::to_string(kBytes)};
        cfg.size.cols = 100;
        cfg.size.rows = 30;
        if (pty->spawn(cfg)) {
            std::string buf(64u << 10, '\0');
            size_t got = 0;
            const auto a = Clock::now();
            for (;;) {
                const size_t n = pty->read(buf.data(), buf.size());
                if (n == 0) break;
                got += n;
            }
            const double secs = std::chrono::duration<double>(Clock::now() - a).count();
            std::printf("  pty only: %.1f MB in %.2f s = %.1f MB/s\n", double(got) / (1 << 20), secs,
                        double(got) / (1 << 20) / secs);
        }
    }
    TermSession s(100, 30);
    const auto t0 = Clock::now();
    if (!spawnChild(s, {"flood", std::to_string(kBytes)})) return;
    // The main thread meanwhile: takes frames and makes control calls, timing
    // each. None may wait on the parser for more than a slice or so.
    double worstControlMs = 0;
    uint64_t framesTaken = 0;
    while (!s.exited() && Clock::now() - t0 < 120s) {
        const auto a = Clock::now();
        if (s.hasNewFrame()) {
            s.acquireFrame();
            ++framesTaken;
        }
        (void)s.cursor();
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - a).count();
        worstControlMs = std::max(worstControlMs, ms);
        std::this_thread::sleep_for(4ms);
    }
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    CHECK(s.exited());
    const auto st = s.stats();
    std::printf("  flood: %.1f MB in %.2f s = %.1f MB/s parsed; %llu frames published, %llu taken; "
                "worst control call %.2f ms\n",
                double(st.bytesParsed) / (1 << 20), secs, double(st.bytesParsed) / (1 << 20) / secs,
                (unsigned long long)st.framesPublished, (unsigned long long)framesTaken, worstControlMs);
    CHECK(st.bytesParsed >= uint64_t(kBytes));
    CHECK_MSG(worstControlMs < 50.0, std::to_string(worstControlMs) + " ms");
    // Nothing lost: the last flood line is the one the byte count implies.
    long long sent = 0, n = 0;
    for (;; ++n) {
        sent += (long long)("flood line " + std::to_string(n) + " ................................................\n").size();
        if (sent >= kBytes) break;
    }
    const std::string screen = s.screenText();
    CHECK_MSG(contains(screen, "flood line " + std::to_string(n) + " ...") && contains(screen, "FLOOD-DONE"),
              screen.substr(screen.size() > 300 ? screen.size() - 300 : 0));
    CHECK(waitFor([&] { return contains(presented(s), "FLOOD-DONE"); }));
}

void testSynchronizedOutput() {
    section("session: synchronized output (mode 2026)");
    {
        TermSession s(40, 6);
        s.feed("before");
        CHECK(waitFor([&] { return contains(presented(s), "before"); }));
        s.feed("\x1b[?2026h\r\nhalf drawn");
        std::this_thread::sleep_for(60ms);
        CHECK_MSG(!contains(presented(s), "half"), presented(s));
        CHECK(contains(s.screenText(), "half drawn"));  // parsed, just not presented
        s.feed(" and done\x1b[?2026l");
        CHECK(waitFor([&] { return contains(presented(s), "half drawn and done"); }, 1s));
        CHECK(s.stats().syncHolds == 1 && s.stats().syncTimeouts == 0);
    }
    {
        // Back to back: each finished update is presented, never the next one
        // half drawn.
        TermSession s(40, 6);
        s.feed("\x1b[?2026hfirst\x1b[?2026l\x1b[?2026h\r\nsecond");
        CHECK(waitFor([&] { return contains(presented(s), "first"); }, 1s));
        CHECK(!contains(presented(s), "second"));
    }
    {
        // A program that never ends its update is shown after the timeout.
        TermSession s(40, 6);
        const auto t0 = Clock::now();
        s.feed("\x1b[?2026hstuck");
        CHECK(waitFor([&] { return contains(presented(s), "stuck"); }, 2s));
        const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);
        CHECK_MSG(held >= TermSession::kSyncTimeout - 5ms, std::to_string(held.count()) + " ms");
        CHECK(s.stats().syncTimeouts == 1);
    }
    {
        // Through a real PTY: the child prints a held update and ends it.
        TermSession s(40, 6);
        if (!spawnChild(s, {"print", "\\e[?2026hsynced\\e[?2026l done"})) return;
        CHECK(waitFor([&] { return s.exited(); }));
        CHECK(waitFor([&] { return contains(presented(s), "synced done"); }));
    }
}

} // namespace

void run_session_tests() {
    testFeed();
    testPrintAndExit();
    testInput();
    testResize();
    testFlood();
    testSynchronizedOutput();
}

} // namespace bro::terminal::test
