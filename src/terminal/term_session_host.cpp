// What the program says to the embedder: bropty's TerminalHost callbacks
// queued as TermEvents for the main thread (ElTerminal turns them into DOM
// events), the OSC 52 clipboard policy, and the session's reads of the
// program-set state (cwd, pointer shape, shell commands, palette).

#include "terminal/term_session_host.h"

#include <algorithm>

namespace bro::terminal {

namespace {

// Events a page never reads pile up no further than this; the oldest go.
constexpr size_t kMaxQueuedEvents = 1024;

} // namespace

std::unique_ptr<TermSession::Host> TermSession::makeHost() { return std::make_unique<Host>(*this); }
bropty::TerminalHost* TermSession::hostDelegate() { return host_.get(); }

void TermSession::pushEvent(TermEvent ev) {
    std::lock_guard<std::mutex> g(evMu_);
    if (!events_.empty()) {
        TermEvent& last = events_.back();
        // A burst of the same state change is one change: the latest wins.
        // (Bells ring once per burst; titles and progress settle on the last.)
        const bool coalesce = last.kind == ev.kind &&
            (ev.kind == TermEvent::Kind::Bell || ev.kind == TermEvent::Kind::Title ||
             ev.kind == TermEvent::Kind::Progress || ev.kind == TermEvent::Kind::PointerShape ||
             ev.kind == TermEvent::Kind::Cwd);
        if (coalesce) {
            last = std::move(ev);
            return;
        }
    }
    if (events_.size() >= kMaxQueuedEvents) events_.erase(events_.begin());
    events_.push_back(std::move(ev));
}

std::vector<TermEvent> TermSession::takeEvents() {
    std::lock_guard<std::mutex> g(evMu_);
    std::vector<TermEvent> out;
    out.swap(events_);
    return out;
}

bool TermSession::answerClipboard(uint64_t request, std::string_view data) {
    std::lock_guard<std::mutex> g(mu_);
    if (clipboardPolicy() != ClipboardPolicy::ReadWrite) {
        session_.terminal().cancel_clipboard(request);
        return false;
    }
    return session_.terminal().answer_clipboard(request, data);
}

bool TermSession::cancelClipboard(uint64_t request) {
    std::lock_guard<std::mutex> g(mu_);
    return session_.terminal().cancel_clipboard(request);
}

std::string TermSession::cwd() const {
    std::lock_guard<std::mutex> g(mu_);
    return session_.terminal().cwd();
}

std::string TermSession::pointerShape() const {
    std::lock_guard<std::mutex> g(mu_);
    return session_.terminal().pointer_shape();
}

std::vector<CommandInfo> TermSession::commands() const {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<CommandInfo> out;
    const auto& list = session_.terminal().commands();
    out.reserve(list.size());
    for (const bropty::CommandRecord& c : list) {
        CommandInfo ci;
        ci.prompt = c.prompt;
        ci.input = c.input;
        ci.output = c.output;
        ci.end = c.end;
        ci.exitCode = c.exit_code;
        ci.finished = c.finished;
        ci.commandLine = c.command_line;
        // A shell that reports no command line (cmdline= / OSC 633;E): what
        // was typed between the end of its prompt (B) and its output (C).
        if (ci.commandLine.empty() && c.input && c.output && !c.trimmed && *c.input < *c.output) {
            bropty::Selection probe(session_.terminal());
            probe.select_range(bropty::RowRange{*c.input, *c.output});
            std::string text = probe.text();
            const size_t a = text.find_first_not_of(" \t\r\n");
            const size_t b = text.find_last_not_of(" \t\r\n");
            ci.commandLine = a == std::string::npos ? std::string() : text.substr(a, b - a + 1);
        }
        out.push_back(std::move(ci));
    }
    return out;
}

void TermSession::setBasePalette(const bropty::Palette& palette) {
    {
        std::lock_guard<std::mutex> g(mu_);
        session_.terminal().set_base_palette(palette);
    }
    wake();  // the next frame carries the new palette
}

bropty::Palette TermSession::palette() const {
    std::lock_guard<std::mutex> g(mu_);
    return session_.terminal().palette();
}

// ---------------------------------------------------------------------------
// TerminalHost

void TermSession::Host::bell() {
    TermEvent e;
    e.kind = TermEvent::Kind::Bell;
    s_.pushEvent(std::move(e));
}

void TermSession::Host::title_changed(std::string_view title) {
    TermEvent e;
    e.kind = TermEvent::Kind::Title;
    e.text = std::string(title);
    s_.pushEvent(std::move(e));
}

void TermSession::Host::cwd_changed(std::string_view uri) {
    TermEvent e;
    e.kind = TermEvent::Kind::Cwd;
    e.text = std::string(uri);
    s_.pushEvent(std::move(e));
}

void TermSession::Host::clipboard_write(std::string_view selection, std::string_view data) {
    if (s_.clipboardPolicy() == ClipboardPolicy::Deny) return;
    TermEvent e;
    e.kind = TermEvent::Kind::ClipboardWrite;
    e.selection = std::string(selection);
    e.text = std::string(data);
    s_.pushEvent(std::move(e));
}

// Never answered at once: a read either goes to the page (async) or is refused.
std::optional<std::string> TermSession::Host::clipboard_read(std::string_view) { return std::nullopt; }

bool TermSession::Host::clipboard_read_async(uint64_t request, std::string_view selection) {
    if (s_.clipboardPolicy() != ClipboardPolicy::ReadWrite) return false;
    TermEvent e;
    e.kind = TermEvent::Kind::ClipboardRead;
    e.request = request;
    e.selection = std::string(selection);
    s_.pushEvent(std::move(e));
    return true;
}

void TermSession::Host::notification_ex(const bropty::Notification& n) {
    TermEvent e;
    e.kind = TermEvent::Kind::Notification;
    e.title = n.title;
    e.text = n.body;
    e.id = n.id;
    e.source = n.source;
    e.number = n.urgency;
    s_.pushEvent(std::move(e));
}

void TermSession::Host::progress(int state, int value) {
    TermEvent e;
    e.kind = TermEvent::Kind::Progress;
    e.number = std::clamp(state, 0, 4);
    e.value = std::clamp(value, 0, 100);
    s_.pushEvent(std::move(e));
}

void TermSession::Host::semantic_mark(char kind, std::string_view params) {
    s_.commandsVersion_.fetch_add(1, std::memory_order_relaxed);
    TermEvent e;
    e.kind = TermEvent::Kind::PromptMark;
    e.mark = kind;
    e.params = std::string(params);
    s_.pushEvent(std::move(e));
}

void TermSession::Host::pointer_shape_changed(std::string_view name) {
    TermEvent e;
    e.kind = TermEvent::Kind::PointerShape;
    e.text = std::string(name);
    s_.pushEvent(std::move(e));
}

} // namespace bro::terminal
