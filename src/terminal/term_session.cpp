#include "terminal/term_session.h"

#include <algorithm>
#include <cstdlib>

namespace bro::terminal {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kReadChunk = 16u << 10;

// The end of a synchronized update: DECRST 2026. Spelled with other modes in
// the same sequence (CSI ? 25;2026 l) it is not seen here, which only costs
// that update its early presentation: the end-of-slice check still sees the
// mode reset and publishes.
constexpr std::string_view kEsu = "\x1b[?2026l";

} // namespace

std::string defaultShell() {
#if defined(_WIN32)
    if (const char* c = std::getenv("COMSPEC"); c && *c) return c;
    return "cmd.exe";
#else
    if (const char* s = std::getenv("SHELL"); s && *s) return s;
    return "/bin/sh";
#endif
}

TermSession::TermSession(int cols, int rows, size_t scrollbackRows)
    : session_([&] {
          bropty::TerminalOptions o;
          o.cols = std::max(1, cols);
          o.rows = std::max(1, rows);
          o.scrollback_rows = scrollbackRows;
          return o;
      }()),
      readBuf_(std::make_unique<char[]>(kReadChunk)),
      cols_(std::max(1, cols)),
      rows_(std::max(1, rows)) {
    view_ = std::make_unique<bropty::TerminalView>(session_.terminal());
    // A first frame before the thread exists, so a terminal that never gets a
    // process still paints its (empty) screen.
    if (view_->publish(channel_)) framesPublished_.fetch_add(1, std::memory_order_relaxed);
    thread_ = std::thread([this] { threadMain(); });
}

TermSession::~TermSession() {
    stop_.store(true, std::memory_order_release);
    wake();
    if (thread_.joinable()) thread_.join();
    // Outside every lock: teardown is bounded but may take the grace periods,
    // and the PTY's threads call wake() until they are joined.
    std::shared_ptr<bropty::IPtyProcess> pty;
    {
        std::lock_guard<std::mutex> g(mu_);
        pty = pty_;
    }
    if (pty) pty->terminate();
}

bool TermSession::spawn(const SpawnOptions& opts, std::string* error) {
    std::shared_ptr<bropty::IPtyProcess> pty;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (pty_) {
            if (error) *error = "this terminal already has a process";
            return false;
        }
        bropty::PtyConfig cfg;
        cfg.command = opts.command.empty() ? defaultShell() : opts.command;
        cfg.args = opts.args;
        cfg.cwd = opts.cwd;
        cfg.env = opts.env;
        const bropty::Terminal& t = session_.terminal();
        cfg.size.cols = t.cols();
        cfg.size.rows = t.rows();
        cfg.size.pixel_width = t.cols() * std::max(0, t.cell_pixel_width());
        cfg.size.pixel_height = t.rows() * std::max(0, t.cell_pixel_height());
        pty = bropty::create_pty();
        pty->set_wakeup([this] { wake(); });
        if (!pty->spawn(cfg)) {
            if (error) *error = pty->last_error();
            return false;
        }
        pty_ = pty;
        session_.attach_pty(pty);
        pid_.store(pty->pid(), std::memory_order_relaxed);
        spawned_.store(true, std::memory_order_release);
    }
    wake();
    return true;
}

std::optional<int> TermSession::exitCode() const {
    if (!haveExitCode_.load(std::memory_order_acquire)) return std::nullopt;
    return exitCode_.load(std::memory_order_relaxed);
}

void TermSession::kill() {
    std::shared_ptr<bropty::IPtyProcess> pty;
    {
        std::lock_guard<std::mutex> g(mu_);
        pty = pty_;
    }
    if (pty) pty->terminate();
    wake();
}

bool TermSession::write(std::string_view bytes) {
    std::lock_guard<std::mutex> g(mu_);
    if (!pty_ || exited()) return false;
    return pty_->write(bytes) == bytes.size();
}

void TermSession::feed(std::string_view output) {
    {
        std::lock_guard<std::mutex> g(mu_);
        feedSplitting(output, Clock::now());
        bytesParsed_.fetch_add(output.size(), std::memory_order_relaxed);
    }
    wake();
}

bool TermSession::sendKey(const bropty::KeyEvent& ev) {
    std::lock_guard<std::mutex> g(mu_);
    if (!pty_ || exited()) return false;
    return session_.send_key(ev);
}

bool TermSession::sendText(std::string_view text) {
    std::lock_guard<std::mutex> g(mu_);
    if (!pty_ || exited()) return false;
    return session_.send_text(text);
}

bool TermSession::paste(std::string_view text) {
    std::lock_guard<std::mutex> g(mu_);
    if (!pty_ || exited()) return false;
    return session_.paste(text);
}

bool TermSession::focus(bool focused) {
    std::lock_guard<std::mutex> g(mu_);
    if (!pty_ || exited()) return false;
    return session_.focus(focused);
}

bool TermSession::resize(int cols, int rows, int cellPxW, int cellPxH) {
    cols = std::max(1, cols);
    rows = std::max(1, rows);
    bool changed = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        bropty::Terminal& t = session_.terminal();
        if (cols != t.cols() || rows != t.rows()) {
            session_.resize(cols, rows);
            changed = true;
        }
        // Only on a change: the pixel size reaches the PTY's window size too,
        // and the element calls this every frame.
        if (cellPxW > 0 && cellPxH > 0 && (cellPxW != cellPxW_ || cellPxH != cellPxH_)) {
            session_.set_cell_pixel_size(cellPxW, cellPxH);
            cellPxW_ = cellPxW;
            cellPxH_ = cellPxH;
        }
        cols_.store(t.cols(), std::memory_order_relaxed);
        rows_.store(t.rows(), std::memory_order_relaxed);
    }
    if (changed) wake();
    return changed;
}

std::string TermSession::screenText() const {
    std::lock_guard<std::mutex> g(mu_);
    const bropty::Terminal& t = session_.terminal();
    std::vector<std::string> lines;
    lines.reserve(size_t(t.rows()));
    for (int y = 0; y < t.rows(); ++y) lines.push_back(t.row_text(y));
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) out.push_back('\n');
        out += lines[i];
    }
    return out;
}

std::string TermSession::scrollbackText() const {
    std::lock_guard<std::mutex> g(mu_);
    const bropty::Terminal& t = session_.terminal();
    std::string out;
    const size_t n = t.history_rows();
    for (size_t i = 0; i < n; ++i) {
        if (i) out.push_back('\n');
        out += t.history_text(i);
    }
    return out;
}

bropty::CursorState TermSession::cursor() const {
    std::lock_guard<std::mutex> g(mu_);
    return session_.terminal().cursor();
}

bropty::Modes TermSession::modes() const {
    std::lock_guard<std::mutex> g(mu_);
    return session_.terminal().modes();
}

std::string TermSession::title() const {
    std::lock_guard<std::mutex> g(mu_);
    return session_.terminal().title();
}

uint32_t TermSession::kittyKeyboardFlags() const {
    std::lock_guard<std::mutex> g(mu_);
    return session_.terminal().kitty_keyboard_flags();
}

std::string TermSession::selectionText() const {
    std::lock_guard<std::mutex> g(mu_);
    const bropty::Selection& sel = view_->selection();
    return sel.active() ? sel.text() : std::string();
}

void TermSession::select(bropty::RowRange range) {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (range.empty()) view_->selection().clear();
        else view_->selection().select_range(range);
    }
    wake();
}

std::shared_ptr<const bropty::Frame> TermSession::acquireFrame() {
    auto f = channel_.acquire();
    if (publishOwed_.exchange(false, std::memory_order_acq_rel)) wake();
    return f;
}

TermSession::Stats TermSession::stats() const noexcept {
    Stats s;
    s.bytesParsed = bytesParsed_.load(std::memory_order_relaxed);
    s.framesPublished = framesPublished_.load(std::memory_order_relaxed);
    s.syncHolds = syncHolds_.load(std::memory_order_relaxed);
    s.syncTimeouts = syncTimeouts_.load(std::memory_order_relaxed);
    return s;
}

// ---------------------------------------------------------------------------
// The parser thread

void TermSession::wake() {
    // The PTY's reader calls this for every chunk it buffers: coalesce, so a
    // flood costs one notification per drain rather than one per read.
    if (wakePending_.exchange(true, std::memory_order_acq_rel)) return;
    {
        std::lock_guard<std::mutex> g(wakeMu_);
        wakeFlag_ = true;
    }
    wakeCv_.notify_one();
}

void TermSession::threadMain() {
    while (!stop_.load(std::memory_order_acquire)) {
        // Sleep until woken, or until a held synchronized update times out.
        Clock::time_point holdUntil{};
        {
            std::lock_guard<std::mutex> g(mu_);
            if (syncActive_ && !syncTimedOut_) holdUntil = syncSince_ + kSyncTimeout;
        }
        {
            std::unique_lock<std::mutex> lk(wakeMu_);
            auto ready = [this] { return wakeFlag_ || stop_.load(std::memory_order_acquire); };
            if (holdUntil != Clock::time_point{}) wakeCv_.wait_until(lk, holdUntil, ready);
            else wakeCv_.wait(lk, ready);
            wakeFlag_ = false;
        }
        wakePending_.store(false, std::memory_order_release);
        if (stop_.load(std::memory_order_acquire)) break;

        // Drain in slices, letting go of the lock between them so input and
        // resizes from the main thread get in.
        for (;;) {
            bool published = false;
            bool more;
            {
                std::lock_guard<std::mutex> g(mu_);
                more = parseSlice(Clock::now(), published);
            }
            if (!more || stop_.load(std::memory_order_acquire)) break;
            std::this_thread::yield();
        }
    }
}

bool TermSession::parseSlice(Clock::time_point now, bool& published) {
    const Clock::time_point deadline = now + kSliceTime;
    bool pending = false;
    if (pty_) {
        // Moves a paste larger than the PTY's input queue along (bropty
        // keeps the rest in the Session's outbox); reads nothing.
        bropty::Session::UpdateBudget flushOnly;
        flushOnly.max_bytes = 0;
        session_.update(flushOnly);

        size_t total = 0;
        for (;;) {
            const size_t n = pty_->read_nonblocking(readBuf_.get(), kReadChunk);
            if (n == 0) break;
            feedSplitting(std::string_view(readBuf_.get(), n), now);
            total += n;
            if (Clock::now() >= deadline) break;
        }
        bytesParsed_.fetch_add(total, std::memory_order_relaxed);
        pending = pty_->available() > 0;
    }
    // Publish only once the renderer has taken the last frame: a flood then
    // builds at most one frame per frame drawn, not one per slice (which
    // spent most of the parser's time building frames nobody saw). What is
    // not published now is owed, and paid as soon as the renderer takes the
    // frame it has (acquireFrame), so the final state is never left behind.
    published = maybePublish(Clock::now(), /*onlyIfConsumed=*/true);

    if (pty_ && !pending && !exited() && pty_->eof()) {
        if (auto code = pty_->exit_code()) {
            exitCode_.store(*code, std::memory_order_relaxed);
            haveExitCode_.store(true, std::memory_order_release);
        }
        exited_.store(true, std::memory_order_release);
    }
    return pending;
}

void TermSession::feedSplitting(std::string_view chunk, Clock::time_point now) {
    (void)now;
    size_t start = 0;
    for (size_t i = 0; i < chunk.size(); ++i) {
        const char c = chunk[i];
        if (c == kEsu[size_t(esuMatch_)]) {
            if (++esuMatch_ == int(kEsu.size())) {
                esuMatch_ = 0;
                session_.feed(chunk.substr(start, i + 1 - start));
                start = i + 1;
                // The update is complete: present it before the next begins.
                maybePublish(Clock::now(), /*onlyIfConsumed=*/false);
            }
        } else {
            esuMatch_ = (c == kEsu[0]) ? 1 : 0;
        }
    }
    if (start < chunk.size()) session_.feed(chunk.substr(start));
}

bool TermSession::maybePublish(Clock::time_point now, bool onlyIfConsumed) {
    const bool sync = session_.terminal().modes().synchronized_output;
    if (sync) {
        if (!syncActive_) {
            syncActive_ = true;
            syncTimedOut_ = false;
            syncSince_ = now;
            syncHolds_.fetch_add(1, std::memory_order_relaxed);
        }
        if (now - syncSince_ < kSyncTimeout) return false;
        if (!syncTimedOut_) {
            syncTimedOut_ = true;
            syncTimeouts_.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        syncActive_ = false;
        syncTimedOut_ = false;
    }
    if (onlyIfConsumed && channel_.has_new()) {
        // The renderer has not taken the last frame yet: building another
        // now would only replace it unseen. Owe it instead; acquireFrame()
        // wakes this thread to pay once the renderer takes the one it has.
        publishOwed_.store(true, std::memory_order_release);
        return false;
    }
    publishOwed_.store(false, std::memory_order_relaxed);
    if (!view_->publish(channel_)) return false;
    framesPublished_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace bro::terminal
