#pragma once
// TermSession: one terminal's emulator, its child process and the thread that
// joins them. What a <terminal> element (layout/el_terminal.h) drives and
// paints; nothing here knows about the DOM.
//
// Threading, under bro's policy (data plane lock-free, control plane may
// lock):
//
//   * bropty's PTY owns a reader thread that copies the child's output into a
//     bounded ring (backpressure: a full ring stops the reader, the OS pipe
//     fills, the child blocks in write()). Its wakeup hook wakes the parser
//     thread below.
//   * The PARSER THREAD (one per session) drains the ring into the bropty
//     Terminal in slices of at most kSliceTime, under `mu_`, and publishes
//     immutable bropty::Frames through a lock-free FrameChannel. Output is
//     never parsed on the main thread, so a flood cannot stall a frame; and
//     nothing is dropped, because what the parser has not consumed stays in
//     the ring and throttles the producer instead.
//   * The MAIN THREAD reads frames lock-free (acquireFrame) and paints them.
//     Input (keys, text, paste, focus reports), resize and the test reads
//     take `mu_`: they are rare, and the parser holds the lock for one slice
//     at most, so a keypress waits a couple of milliseconds in the worst case.
//
// Synchronized output (DEC mode 2026): while the program holds an update open
// nothing is published, so the screen keeps showing the last complete
// update; the hold ends when the program resets the mode or after
// kSyncTimeout. The parser cuts its input at every end-of-update sequence and
// publishes right there, so a program that opens the next update at once
// still has each finished one presented, never a half-drawn successor.
//
// bro's main loop runs continuously at its frame cap (it does not block in
// the OS event queue), so a published frame is picked up by the next frame's
// pump without any cross-thread wakeup of the main thread; the parser only
// has to be woken itself, which the PTY's hook does.

#include <bropty/frame.h>
#include <bropty/input.h>
#include <bropty/pty.h>
#include <bropty/session.h>
#include <bropty/view.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace bro::terminal {

struct SpawnOptions {
    std::string command;            // empty: the platform's default shell
    std::vector<std::string> args;
    std::string cwd;                // empty: inherit
    std::vector<std::pair<std::string, std::string>> env;  // on top of the inherited environment
};

// The default shell spawn() runs for an empty command: %COMSPEC% (else
// cmd.exe) on Windows, $SHELL (else /bin/sh) elsewhere.
std::string defaultShell();

class TermSession {
public:
    static constexpr std::chrono::milliseconds kSyncTimeout{200};
    static constexpr std::chrono::microseconds kSliceTime{2000};

    TermSession(int cols, int rows, size_t scrollbackRows = 10000);
    ~TermSession();
    TermSession(const TermSession&) = delete;
    TermSession& operator=(const TermSession&) = delete;

    // ---- control plane (main thread) -----------------------------------------
    // Start a child on a new PTY sized like the terminal. One per session.
    bool spawn(const SpawnOptions& opts, std::string* error);
    [[nodiscard]] bool spawned() const noexcept { return spawned_.load(std::memory_order_acquire); }
    // The child has exited and every byte of its output has been parsed.
    [[nodiscard]] bool exited() const noexcept { return exited_.load(std::memory_order_acquire); }
    [[nodiscard]] bool running() const noexcept { return spawned() && !exited(); }
    [[nodiscard]] std::optional<int> exitCode() const;
    [[nodiscard]] int64_t pid() const noexcept { return pid_.load(std::memory_order_relaxed); }
    // Stop the child (bropty's bounded escalation). The session keeps its screen.
    void kill();

    // Raw bytes to the child's input, unencoded. All or nothing.
    bool write(std::string_view bytes);
    // Bytes into the emulator as if the program had written them.
    void feed(std::string_view output);
    // Encoded for the terminal's current modes (bropty::Session).
    bool sendKey(const bropty::KeyEvent& ev);
    bool sendText(std::string_view text);
    bool paste(std::string_view text);
    // Focus report (only sent when the program set ?1004).
    bool focus(bool focused);
    // Grid and cell pixel size; resizes the PTY too. True when the grid changed.
    bool resize(int cols, int rows, int cellPxW, int cellPxH);
    [[nodiscard]] int cols() const noexcept { return cols_.load(std::memory_order_relaxed); }
    [[nodiscard]] int rows() const noexcept { return rows_.load(std::memory_order_relaxed); }

    // Live state, read under the lock (tests, the JS surface).
    [[nodiscard]] std::string screenText() const;
    [[nodiscard]] std::string scrollbackText() const;
    [[nodiscard]] bropty::CursorState cursor() const;
    [[nodiscard]] bropty::Modes modes() const;
    [[nodiscard]] std::string title() const;
    [[nodiscard]] uint32_t kittyKeyboardFlags() const;
    // The selected text ("" when nothing is selected).
    [[nodiscard]] std::string selectionText() const;
    // Selection over the view (absolute rows), for painting tests until T2's
    // mouse selection; an empty range clears it.
    void select(bropty::RowRange range);

    // ---- data plane (main thread, lock-free) ---------------------------------
    // The newest published frame (the same one again when nothing is newer).
    // Frames are published at most one per frame taken: newer output than
    // the frame returned is published right after this call (the parser is
    // woken for it), so the next acquire within a few ms has it.
    std::shared_ptr<const bropty::Frame> acquireFrame();
    [[nodiscard]] bool hasNewFrame() const noexcept { return channel_.has_new(); }

    // Counters for perf tests (relaxed atomics, any thread).
    struct Stats {
        uint64_t bytesParsed = 0;
        uint64_t framesPublished = 0;
        uint64_t syncHolds = 0;      // synchronized updates that held presentation
        uint64_t syncTimeouts = 0;   // ... and ended by the timeout
    };
    [[nodiscard]] Stats stats() const noexcept;

private:
    void threadMain();
    void wake();
    // One parse slice under mu_. Returns whether more output is pending.
    bool parseSlice(std::chrono::steady_clock::time_point now, bool& published);
    // Publish if presentation is not held by a synchronized update.
    bool maybePublish(std::chrono::steady_clock::time_point now, bool onlyIfConsumed);
    // Feed `chunk`, cutting it after each end-of-synchronized-update so the
    // finished update is published before the next one starts.
    void feedSplitting(std::string_view chunk, std::chrono::steady_clock::time_point now);

    mutable std::mutex mu_;  // guards session_, view_, the sync state
    bropty::Session session_;
    std::unique_ptr<bropty::TerminalView> view_;
    bropty::FrameChannel channel_;
    std::shared_ptr<bropty::IPtyProcess> pty_;
    std::unique_ptr<char[]> readBuf_;

    // Synchronized-output state (mu_).
    bool syncActive_ = false;
    std::chrono::steady_clock::time_point syncSince_{};
    bool syncTimedOut_ = false;
    int esuMatch_ = 0;  // bytes of "\x1b[?2026l" matched at the end of the last chunk
    int cellPxW_ = 0, cellPxH_ = 0;  // the cell pixel size last given (mu_)

    std::atomic<int> cols_;
    std::atomic<int> rows_;
    std::atomic<bool> spawned_{false};
    std::atomic<bool> exited_{false};
    std::atomic<int64_t> pid_{0};
    std::atomic<int> exitCode_{0};
    std::atomic<bool> haveExitCode_{false};

    std::atomic<uint64_t> bytesParsed_{0};
    std::atomic<uint64_t> framesPublished_{0};
    std::atomic<uint64_t> syncHolds_{0};
    std::atomic<uint64_t> syncTimeouts_{0};

    std::mutex wakeMu_;
    std::condition_variable wakeCv_;
    bool wakeFlag_ = false;  // wakeMu_
    std::atomic<bool> wakePending_{false};
    // A publish was skipped because the renderer had not taken the previous
    // frame; acquireFrame() wakes the parser to make it.
    std::atomic<bool> publishOwed_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace bro::terminal
