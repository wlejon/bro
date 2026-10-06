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

#include "terminal/term_types.h"

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

namespace bromux {
class ScreenModel;
}

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
    static constexpr std::chrono::microseconds kSearchStep{1000};

    TermSession(int cols, int rows, size_t scrollbackRows = 10000);
    ~TermSession();
    TermSession(const TermSession&) = delete;
    TermSession& operator=(const TermSession&) = delete;

    // ---- control plane (main thread) -----------------------------------------
    // Start a child on a new PTY sized like the terminal. One per session
    // (spawn, spawnPersistent and attach alike).
    bool spawn(const SpawnOptions& opts, std::string* error);
    [[nodiscard]] bool spawned() const noexcept { return spawned_.load(std::memory_order_acquire); }
    // The child has exited and every byte of its output has been parsed.
    [[nodiscard]] bool exited() const noexcept { return exited_.load(std::memory_order_acquire); }
    [[nodiscard]] bool running() const noexcept { return spawned() && !exited() && !detached(); }

    // ---- persistent sessions (term_session_mux.cpp; term_mux.h) -------------
    // The program runs in a bromux server's session instead of on a PTY of
    // this process: the session keeps running when this TermSession goes
    // (it detaches), and any number of TermSessions -- in this process or
    // others -- attach to it by id. The screen is the server's, mirrored
    // (bromux's ScreenSource); input, resize, mouse, focus and the
    // clipboard go through bromux's protocol, and the program's events come
    // back through it. With a protocol 2.1 server the foreground process,
    // OSC 133 command records, the OSC 22 pointer shape, inline images and
    // feed() cross too (term_session_mux_state.cpp); a 2.0 server carries
    // none of them, and they stay empty.
    struct PersistentOptions {
        std::string server;  // bromux server name; empty: the per-user default
        std::string name;    // the session's display name (its "name" meta)
    };
    // Create a session running `opts` and attach to it.
    bool spawnPersistent(const SpawnOptions& opts, const PersistentOptions& p, std::string* error);
    // Attach to an existing session.
    bool attach(uint64_t sessionId, const std::string& server, std::string* error);
    // Let go of the session, which keeps running. The screen stays as it was.
    void detach();
    [[nodiscard]] bool detached() const noexcept { return detached_.load(std::memory_order_acquire); }
    // The session's id while attached to one (0: a local session, or none).
    [[nodiscard]] uint64_t sessionId() const noexcept { return sessionId_.load(std::memory_order_acquire); }
    [[nodiscard]] bool persistent() const noexcept { return persistent_.load(std::memory_order_acquire); }
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
    // Select an explicit stream range (absolute rows); an empty one clears.
    void select(bropty::RowRange range);

    // ---- the view: scrollback position, selection, search, links ---------
    // (term_session_view.cpp). Each takes the lock and wakes the parser,
    // which publishes the change in the next frame.
    [[nodiscard]] ViewState viewState() const;
    void scrollBy(int64_t rows);          // negative: back into history
    void scrollToRow(int64_t row);        // `row` at the top, clamped
    void scrollToBottom();
    void scrollToTop();
    // To the previous / next OSC 133 prompt. False when there is none.
    bool scrollToPrompt(bool backward);
    // Typing and pasting return the view to the bottom (default on).
    void setScrollOnInput(bool on) { scrollOnInput_.store(on, std::memory_order_relaxed); }
    [[nodiscard]] bool scrollOnInput() const { return scrollOnInput_.load(std::memory_order_relaxed); }

    // Selection gestures over absolute cells (see bropty::Selection).
    void selectStart(bropty::RowPos cell, bropty::SelectionMode mode, bool rightHalf);
    void selectExtend(bropty::RowPos cell, bool rightHalf);
    void selectClear();
    void selectAll();
    // The output of the command at `cell` (or the last one). False: none.
    bool selectOutput(std::optional<bropty::RowPos> cell);
    [[nodiscard]] bool selectionActive() const;
    [[nodiscard]] std::optional<bropty::RowRange> selectionRange() const;
    [[nodiscard]] bool selectionIsBlock() const;

    // Search over the screen and history. False with *error on a bad pattern.
    bool searchStart(std::string_view pattern, const SearchOptions& opts, std::string* error);
    void searchClear();
    // The next / previous match (wrapping), brought into view.
    std::optional<bropty::RowRange> searchNext(bool backward);
    [[nodiscard]] SearchStatus searchStatus() const;

    // The link at a cell, and the hovered one (underlined while it is).
    [[nodiscard]] std::optional<LinkInfo> linkAt(bropty::RowPos cell) const;
    void setHover(std::optional<bropty::RowPos> cell);

    // Text of absolute rows [first, end), rows joined by "\n" (no trailing
    // spaces): what a range of the buffer reads as.
    [[nodiscard]] std::string rowsText(int64_t first, int64_t end) const;
    [[nodiscard]] std::string rangeText(bropty::RowRange range) const;

    // ---- the mouse (term_session_view.cpp) ---------------------------------
    // A report for the program's mouse mode; false when it reports nothing
    // (no tracking, or a mode that drops this event).
    bool sendMouse(const bropty::MouseEvent& ev);
    [[nodiscard]] bropty::MouseTracking mouseTracking() const;
    [[nodiscard]] bool altScreen() const;

    // ---- what the program says to the embedder (term_session_host.cpp) -------
    void setClipboardPolicy(ClipboardPolicy p) { clipboardPolicy_.store(p, std::memory_order_relaxed); }
    [[nodiscard]] ClipboardPolicy clipboardPolicy() const { return clipboardPolicy_.load(std::memory_order_relaxed); }
    // Answer / refuse an OSC 52 read request (TermEvent::ClipboardRead).
    bool answerClipboard(uint64_t request, std::string_view data);
    bool cancelClipboard(uint64_t request);
    // Events queued since the last call, oldest first (main thread).
    std::vector<TermEvent> takeEvents();
    [[nodiscard]] std::string cwd() const;
    // The shell's commands (OSC 133), oldest first, and a counter that
    // changes whenever the list does.
    [[nodiscard]] std::vector<CommandInfo> commands() const;
    [[nodiscard]] uint64_t commandsVersion() const { return commandsVersion_.load(std::memory_order_relaxed); }
    // The pointer shape the program asked for (OSC 22), "" for none.
    [[nodiscard]] std::string pointerShape() const;
    // The theme: what the palette is and what the program's resets return to.
    void setBasePalette(const bropty::Palette& palette);
    // The palette in effect: the base, with what the program set (OSC 4 /
    // 10 / 11 / 12) over it; a persistent session's, composed by the mirror.
    [[nodiscard]] bropty::Palette palette() const;

    // ---- host settings (term_session_host.cpp) --------------------------------
    // History capacity in rows, applied to the emulator now (a lower one
    // drops the oldest rows). A persistent session's history is the
    // server's: there this has no effect.
    void setScrollbackRows(size_t rows);
    // The cursor style the program's DECSCUSR 0 and RIS return to, applied
    // now. On a persistent session it is shown while the server's cursor is
    // at bropty's default (MuxSource::setDefaultCursor).
    void setDefaultCursor(bropty::CursorShape shape, bool blink);
    // Changes a persistent session's server reported (bromux frames that
    // advanced its state): the mirror's counterpart of Stats::bytesParsed.
    [[nodiscard]] uint64_t remoteUpdates() const noexcept { return remoteUpdates_.load(std::memory_order_relaxed); }

    // ---- inline images (kitty graphics, sixel, iTerm2) ----------------------
    // Decoded through broimage (term_session_host.cpp), animated on the
    // parser thread, shown in the frames (bropty::Frame::images).
    struct ImageStats {
        size_t images = 0;      // both screens
        size_t placements = 0;  // kitty placements, both screens
        size_t bytes = 0;       // decoded RGBA held
        size_t limit = 0;       // the quota `bytes` is held to
    };
    [[nodiscard]] ImageStats imageStats() const;
    // The decoded-image quota (bropty's storage_limit); lowering it evicts now.
    void setImageMemoryLimit(size_t bytes);

    // ---- the foreground process (term_session_foreground.cpp) --------------
    // What owns the terminal now (bropty's IPtyProcess::foreground_process),
    // as last checked: the parser thread checks shortly after input or
    // output (at most every kForegroundGap, and again twice as things
    // settle) and every kForegroundIdle while quiet, and queues a
    // TermEvent::Foreground when the answer changes. Empty before spawn and
    // once the child has exited.
    static constexpr std::chrono::milliseconds kForegroundGap{250};
    static constexpr std::chrono::milliseconds kForegroundIdle{3000};
    [[nodiscard]] std::optional<bropty::ProcessInfo> foregroundProcess() const;

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
    // The embedder's half of bropty's TerminalHost: the Session forwards
    // here, on whichever thread holds mu_ (term_session_host.cpp).
    class Host;
    friend class Host;
    std::unique_ptr<Host> makeHost();
    bropty::TerminalHost* hostDelegate();
    void pushEvent(TermEvent ev);
    void afterViewChange();  // a gesture moved the view: present it
    bool snapOnInput(bool sent);  // mu_

    void threadMain();
    void wake();
    // One parse slice under mu_. Returns whether more output is pending.
    bool parseSlice(std::chrono::steady_clock::time_point now, bool& published);
    // Run image animations up to `now` (mu_); when the next frame is due,
    // or {} when nothing animates.
    std::chrono::steady_clock::time_point advanceAnimations(std::chrono::steady_clock::time_point now);
    // Check the foreground process when one is due (parser thread, outside
    // mu_); `activity`: input or output since the last call. Returns when
    // the next check is due, {} for none.
    std::chrono::steady_clock::time_point pollForeground(std::chrono::steady_clock::time_point now, bool activity);
    // Input went to the program: the foreground may be about to change.
    void noteInput() {
        fgPoke_.store(true, std::memory_order_relaxed);
        wake();
    }
    // Publish if presentation is not held by a synchronized update.
    bool maybePublish(std::chrono::steady_clock::time_point now, bool onlyIfConsumed);
    // Feed `chunk`, cutting it after each end-of-synchronized-update so the
    // finished update is published before the next one starts.
    void feedSplitting(std::string_view chunk, std::chrono::steady_clock::time_point now);

    // The persistent-session state (term_session_mux.cpp), under mu_.
    struct Mux;
    struct MuxDelete {
        void operator()(Mux* m) const;
    };
    // mu_ held: dispatch what the server sent; publish. Returns whether
    // more is pending.
    bool muxPump(std::chrono::steady_clock::time_point now, bool& published);
    // mu_ held: the bromux-side parts of the reads and the input. Input is
    // refused (false) once detached or exited.
    bool muxAttachLocked(uint64_t id, std::string* error);
    void muxDropView();
    void muxMirrorLost();  // the Client dropped the session's mirror: let go of it
    void muxKill();
    bool muxWrite(std::string_view bytes);
    bool muxSendKey(const bropty::KeyEvent& ev);
    bool muxSendText(std::string_view text, bool paste);
    bool muxFocus(bool focused);
    bool muxMouse(const bropty::MouseEvent& ev);
    void muxResize(int cols, int rows, int cellPxW, int cellPxH);
    bool muxAnswerClipboard(uint64_t request, bool ok, std::string_view data);
    void muxSetBasePalette(const bropty::Palette& palette);
    void muxSetDefaultCursor(bropty::CursorShape shape, bool blink);
    std::string muxTitle() const;
    std::string muxCwd() const;
    uint32_t muxKittyFlags() const;
    std::string muxScrollbackText() const;
    // Protocol 2.1's extras (term_session_mux_state.cpp), mu_ held.
    const bromux::ScreenModel* muxModel() const;  // null unless attached with a mirror
    bool muxFeed(std::string_view bytes);
    std::vector<CommandInfo> muxCommands() const;
    std::string muxPointerShape() const;
    ImageStats muxImageStats() const;
    void muxSetForeground(std::optional<bropty::ProcessInfo> info);  // queues Foreground on a change
    void muxFrameApplied(bool pointerShape, bool commands);
    int muxCellW_ = 0, muxCellH_ = 0;  // the cell size last sent (mu_)
    bool muxSized_ = false;

    mutable std::mutex mu_;  // guards session_, the views, the sync state, mux_
    bropty::Session session_;
    // The view the frames come from: over the local Terminal, or over the
    // mirror of a persistent session (muxView). view_ / src_ point at the
    // one in use.
    std::unique_ptr<bropty::TerminalView> localView_;
    bropty::TerminalView* view_ = nullptr;
    bropty::RowSource* src_ = nullptr;
    std::unique_ptr<Mux, MuxDelete> mux_;
    std::atomic<bool> detached_{false};
    std::atomic<bool> persistent_{false};
    std::atomic<uint64_t> sessionId_{0};
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

    std::unique_ptr<Host> host_;
    std::atomic<ClipboardPolicy> clipboardPolicy_{ClipboardPolicy::WriteOnly};
    std::atomic<bool> scrollOnInput_{true};
    std::atomic<uint64_t> commandsVersion_{0};
    std::string searchPattern_;  // mu_
    mutable std::mutex evMu_;    // after mu_ when both are held
    std::vector<TermEvent> events_;  // evMu_

    // The foreground process: the answer (fgMu_), and the parser thread's
    // schedule for checking it.
    mutable std::mutex fgMu_;
    std::optional<bropty::ProcessInfo> fg_;  // fgMu_
    std::atomic<bool> fgPoke_{false};
    std::chrono::steady_clock::time_point fgDue_{}, fgLast_{};
    int fgTrail_ = 0;  // settle checks still to make after the last activity
    bool fgFinal_ = false;  // the exit has been reported; no more checks

    std::atomic<uint64_t> bytesParsed_{0};
    std::atomic<uint64_t> remoteUpdates_{0};
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
