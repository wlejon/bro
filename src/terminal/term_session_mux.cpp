// TermSession over a bromux session (term_session.h, "persistent
// sessions"): the program runs in a per-user bromux server, and this
// session mirrors it. The bromux Client is single-threaded; here every call
// into it is made under mu_ (the parser thread dispatches what arrives, the
// main thread's input and control calls send), which serialises them.
//
// The mirror the view reads (the Client's ScreenSource for the session) goes
// away when the server says the session is detached -- which the Client
// applies inside dispatch() and inside any blocking request -- so after each
// of those the view is dropped first if the mirror has gone (muxCheckMirror).

#include "terminal/term_mux_connect.h"
#include "terminal/term_mux_impl.h"

#include <bromux/paths.h>

#include <algorithm>

namespace bro::terminal {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kHistoryChunk = 2000;  // rows per request when reading all of history

} // namespace

void TermSession::MuxDelete::operator()(Mux* m) const { delete m; }

namespace {

bool connectMux(std::unique_ptr<bromux::Client>& out, const std::string& server, bool autostart,
                std::string* error) {
    if (!server.empty() && !bromux::valid_server_name(server)) {
        if (error) *error = "invalid server name (1-64 of A-Z a-z 0-9 _ . -)";
        return false;
    }
    std::string why;
    out = bromux::Client::connect(muxConnectOptions(server, autostart), &why);
    if (!out && error) *error = "cannot reach the bromux server: " + why;
    return out != nullptr;
}

} // namespace

bool TermSession::spawnPersistent(const SpawnOptions& opts, const PersistentOptions& p, std::string* error) {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (pty_ || mux_) {
            if (error) *error = "this terminal already has a process";
            return false;
        }
        std::unique_ptr<Mux, MuxDelete> m(new Mux);
        if (!connectMux(m->client, p.server, /*autostart=*/true, error)) return false;
        bromux::SessionSpec spec;
        spec.command = opts.command;
        spec.args = opts.args;
        spec.cwd = opts.cwd;
        spec.env = opts.env;
        if (!p.name.empty()) spec.meta.emplace_back("name", p.name);
        const bropty::Terminal& t = session_.terminal();
        spec.cols = uint16_t(std::clamp(t.cols(), 1, 0xFFFF));
        spec.rows = uint16_t(std::clamp(t.rows(), 1, 0xFFFF));
        std::optional<bromux::SessionInfo> info = m->client->create_session(spec, error);
        if (!info) return false;
        mux_ = std::move(m);
        if (!muxAttachLocked(info->id, error)) {
            mux_.reset();
            return false;
        }
    }
    wake();
    return true;
}

bool TermSession::attach(uint64_t sessionId, const std::string& server, std::string* error) {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (pty_ || mux_) {
            if (error) *error = "this terminal already has a process";
            return false;
        }
        std::unique_ptr<Mux, MuxDelete> m(new Mux);
        // Attaching names a session that exists, so its server runs: never start one.
        if (!connectMux(m->client, server, /*autostart=*/false, error)) return false;
        mux_ = std::move(m);
        if (!muxAttachLocked(sessionId, error)) {
            mux_.reset();
            return false;
        }
    }
    wake();
    return true;
}

bool TermSession::muxAttachLocked(uint64_t id, std::string* error) {
    Mux& m = *mux_;
    const bropty::Terminal& t = session_.terminal();
    std::optional<bromux::SessionInfo> info = m.client->attach(id, t.cols(), t.rows(), 0, error);
    if (!info) return false;
    bropty::RowSource* inner = m.client->source(id);
    if (!inner) {
        if (error) *error = "the session went away while attaching";
        return false;
    }
    m.id = id;
    m.attached = true;
    m.source = std::make_unique<MuxSource>(*inner);
    m.source->setBase(t.palette());  // the theme, as the element last gave it
    m.source->setDefaultCursor(t.default_cursor_shape(), t.default_cursor_blink());
    // The attach synced the screen: changes count from here (remoteUpdates_).
    if (const bromux::ScreenModel* s = m.client->screen(id)) m.feedSeq = s->feed_seq();
    m.view = std::make_unique<bropty::TerminalView>(*m.source);
    view_ = m.view.get();
    src_ = m.source.get();
    m.client->set_wakeup([this] { wake(); });
    pid_.store(info->pid, std::memory_order_relaxed);
    sessionId_.store(id, std::memory_order_release);
    persistent_.store(true, std::memory_order_release);
    spawned_.store(true, std::memory_order_release);
    muxSized_ = false;  // the element's size goes out with its next resize()
    if (!info->running) {
        if (info->exit_code >= 0) {
            exitCode_.store(info->exit_code, std::memory_order_relaxed);
            haveExitCode_.store(true, std::memory_order_release);
        }
        exited_.store(true, std::memory_order_release);
    }
    // The attach brought the whole screen: show it now.
    maybePublish(Clock::now(), /*onlyIfConsumed=*/false);
    return true;
}

void TermSession::detach() {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (!mux_ || !mux_->attached) return;
        // The view first: it reads the mirror, which the server's answer drops.
        muxDropView();
        mux_->client->detach(mux_->id);
        mux_->attached = false;
        detached_.store(true, std::memory_order_release);
        muxSetForeground(std::nullopt);
    }
    wake();
}

void TermSession::muxDropView() {
    Mux& m = *mux_;
    m.view.reset();
    m.source.reset();
    view_ = localView_.get();
    src_ = &view_->source();
}

namespace {

// The mirror is gone when the Client no longer has the session's model.
bool mirrorGone(bromux::Client& c, uint64_t id) { return c.screen(id) == nullptr; }

} // namespace

bool TermSession::muxPump(Clock::time_point now, bool& published) {
    Mux& m = *mux_;
    published = false;
    if (!m.client || !m.attached) return false;
    m.events.clear();
    m.client->dispatch(m.events);
    bool ended = false;
    for (bromux::ClientEvent& e : m.events) {
        using K = bromux::ClientEvent::Kind;
        if (e.kind != K::Disconnected && e.session != m.id) continue;
        switch (e.kind) {
            case K::Event: {
                const bromux::EventMsg& ev = e.event;
                TermEvent te;
                switch (ev.kind) {
                    case bromux::EventKind::Bell: te.kind = TermEvent::Kind::Bell; break;
                    case bromux::EventKind::Title:
                        te.kind = TermEvent::Kind::Title;
                        te.text = ev.a;
                        break;
                    case bromux::EventKind::Cwd:
                        te.kind = TermEvent::Kind::Cwd;
                        te.text = ev.a;
                        break;
                    case bromux::EventKind::Notification:
                        te.kind = TermEvent::Kind::Notification;
                        te.title = ev.a;
                        te.text = ev.b;
                        if (m.client->server_minor() >= 1) {  // 2.1 carries OSC 99's fields
                            te.id = ev.c;
                            te.source = ev.d;
                            te.number = int(ev.x);
                        } else {
                            te.source = "bromux";
                        }
                        break;
                    case bromux::EventKind::Foreground: {
                        std::optional<bropty::ProcessInfo> fg;
                        if (ev.x > 0) {
                            fg.emplace();
                            fg->pid = ev.x;
                            fg->name = ev.a;
                            fg->path = ev.b;
                            fg->command_line = ev.c;
                        }
                        muxSetForeground(std::move(fg));
                        continue;
                    }
                    case bromux::EventKind::Progress:
                        te.kind = TermEvent::Kind::Progress;
                        te.number = int(std::clamp<int64_t>(ev.x, 0, 4));
                        te.value = int(std::clamp<int64_t>(ev.y, 0, 100));
                        break;
                    case bromux::EventKind::SemanticMark:
                        commandsVersion_.fetch_add(1, std::memory_order_relaxed);
                        te.kind = TermEvent::Kind::PromptMark;
                        te.mark = char(ev.x);
                        te.params = ev.a;
                        break;
                    case bromux::EventKind::ClipboardWrite:
                        if (clipboardPolicy() == ClipboardPolicy::Deny) continue;
                        te.kind = TermEvent::Kind::ClipboardWrite;
                        te.selection = ev.a;
                        te.text = ev.b;
                        break;
                    case bromux::EventKind::Exited:
                        if (ev.x >= 0) {
                            exitCode_.store(int(ev.x), std::memory_order_relaxed);
                            haveExitCode_.store(true, std::memory_order_release);
                        }
                        ended = true;
                        continue;
                    default: continue;
                }
                pushEvent(std::move(te));
                break;
            }
            case K::ClipboardRequest:
                if (clipboardPolicy() != ClipboardPolicy::ReadWrite) {
                    m.client->answer_clipboard(m.id, e.token, false, {});
                } else {
                    TermEvent te;
                    te.kind = TermEvent::Kind::ClipboardRead;
                    te.request = e.token;
                    te.selection = e.text;
                    pushEvent(std::move(te));
                }
                break;
            case K::Frame:
                muxFrameApplied(e.effects.pointer_shape, e.effects.commands);
                break;
            case K::Detached:
            case K::Disconnected:
                // Closed, or the server went: the program is gone. (A detach
                // this session asked for dropped the view before it was sent.)
                if (e.kind == K::Disconnected && !mirrorGone(*m.client, m.id)) muxDropView();
                muxMirrorLost();
                if (e.kind == K::Disconnected || e.detach_reason != bromux::DetachReason::Requested) ended = true;
                break;
            default: break;
        }
        if (!m.attached) break;
    }
    if (m.attached) {
        // A frame that advanced the server's state is the program's output
        // (or a resize) reaching this mirror: the element's `activity`.
        if (const bromux::ScreenModel* s = m.client->screen(m.id); s && s->feed_seq() != m.feedSeq) {
            m.feedSeq = s->feed_seq();
            remoteUpdates_.fetch_add(1, std::memory_order_relaxed);
        }
        published = maybePublish(now, /*onlyIfConsumed=*/true);
    }
    if (ended) exited_.store(true, std::memory_order_release);
    return false;
}

// ---- input and control (main thread, mu_ held) ---------------------------------

namespace {

bool live(const TermSession& s) { return !s.exited() && !s.detached(); }

} // namespace

void TermSession::muxKill() {
    Mux& m = *mux_;
    if (!m.attached) return;
    m.client->close_session(m.id, nullptr);
    if (mirrorGone(*m.client, m.id)) muxMirrorLost();
    exited_.store(true, std::memory_order_release);
}

bool TermSession::muxWrite(std::string_view bytes) {
    if (!mux_->attached || !live(*this)) return false;
    mux_->client->send_raw(mux_->id, bytes);
    return true;
}

bool TermSession::muxSendKey(const bropty::KeyEvent& ev) {
    if (!mux_->attached || !live(*this)) return false;
    mux_->client->send_key(mux_->id, ev);
    return true;
}

bool TermSession::muxSendText(std::string_view text, bool paste) {
    if (!mux_->attached || !live(*this)) return false;
    if (paste) mux_->client->paste(mux_->id, text);
    else mux_->client->send_text(mux_->id, text);
    return true;
}

bool TermSession::muxFocus(bool focused) {
    if (!mux_->attached || !live(*this)) return false;
    // Reported only when the program asked (?1004), as the server's Session decides.
    mux_->client->focus(mux_->id, focused);
    return src_->modes().focus_events;
}

bool TermSession::muxMouse(const bropty::MouseEvent& ev) {
    if (!mux_->attached || !live(*this)) return false;
    if (src_->modes().mouse_tracking == bropty::MouseTracking::None) return false;
    mux_->client->send_mouse(mux_->id, ev);
    return true;
}

void TermSession::muxResize(int cols, int rows, int cellPxW, int cellPxH) {
    if (!mux_->attached) return;
    mux_->client->resize(mux_->id, cols, rows, cellPxW, cellPxH);
}

bool TermSession::muxAnswerClipboard(uint64_t request, bool ok, std::string_view data) {
    if (!mux_->attached) return false;
    mux_->client->answer_clipboard(mux_->id, uint32_t(request), ok, ok ? data : std::string_view());
    return ok;
}

void TermSession::muxSetBasePalette(const bropty::Palette& palette) {
    if (mux_->source) mux_->source->setBase(palette);
}

void TermSession::muxSetDefaultCursor(bropty::CursorShape shape, bool blink) {
    if (mux_->source) mux_->source->setDefaultCursor(shape, blink);
}

std::string TermSession::muxTitle() const {
    const bromux::ScreenModel* s = mux_->attached ? mux_->client->screen(mux_->id) : nullptr;
    return s ? s->title() : std::string();
}

std::string TermSession::muxCwd() const {
    const bromux::ScreenModel* s = mux_->attached ? mux_->client->screen(mux_->id) : nullptr;
    return s ? s->cwd() : std::string();
}

uint32_t TermSession::muxKittyFlags() const {
    const bromux::ScreenModel* s = mux_->attached ? mux_->client->screen(mux_->id) : nullptr;
    return s ? s->modes().kitty_flags : 0;
}

// All of history, fetched from the server (the mirror holds only what was
// asked for), rows joined by "\n" as the local session's scrollbackText.
std::string TermSession::muxScrollbackText() const {
    Mux& m = *mux_;
    if (!m.attached) return {};
    const bromux::ScreenModel* s = m.client->screen(m.id);
    if (!s) return {};
    const int64_t first = s->history_first_row();
    const int64_t end = s->screen_top_row();
    std::string out;
    bool any = false;
    for (int64_t at = first; at < end;) {
        const uint32_t count = uint32_t(std::min<int64_t>(kHistoryChunk, end - at));
        std::optional<bromux::HistoryChunk> chunk = m.client->fetch_history(m.id, uint64_t(at), count, nullptr);
        if (!chunk || chunk->rows.empty()) break;
        for (size_t i = 0; i < chunk->rows.size() && chunk->start + int64_t(i) < end; ++i) {
            if (any) out.push_back('\n');
            out += chunk->row(i).text();
            any = true;
        }
        at = chunk->start + int64_t(chunk->rows.size());
        // A blocking request applies whatever else arrived: maybe a detach.
        if (mirrorGone(*m.client, m.id)) {
            const_cast<TermSession*>(this)->muxMirrorLost();
            break;
        }
    }
    return out;
}

void TermSession::muxMirrorLost() {
    Mux& m = *mux_;
    if (m.source) m.source->orphan();
    muxDropView();
    m.attached = false;
    detached_.store(true, std::memory_order_release);
    muxSetForeground(std::nullopt);  // nothing of the session's is known from here
}

} // namespace bro::terminal
