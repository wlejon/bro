// TermSession over a bromux session: what crosses beyond the screen with
// protocol 2.1 (bromux docs/protocol.md) -- the foreground process, OSC 133
// command records, the OSC 22 pointer shape, inline images, feed(). The
// screen model (bromux::ScreenModel) holds the records, the pointer shape
// and the images (which the mirror's RowSource offers its view, so frames
// show them); the foreground process comes as events. A 2.0 server carries
// none of it: those reads stay empty and feed() does nothing. Everything
// here runs with mu_ held, like the rest of term_session_mux.cpp.

#include "terminal/term_mux_impl.h"

#include <bropty/graphics.h>

namespace bro::terminal {

const bromux::ScreenModel* TermSession::muxModel() const {
    if (!mux_ || !mux_->client || !mux_->attached) return nullptr;
    return mux_->client->screen(mux_->id);
}

bool TermSession::muxFeed(std::string_view bytes) {
    if (!mux_->attached || exited()) return false;
    return mux_->client->feed(mux_->id, bytes);
}

std::vector<CommandInfo> TermSession::muxCommands() const {
    std::vector<CommandInfo> out;
    const bromux::ScreenModel* s = muxModel();
    if (!s) return out;
    out.reserve(s->commands().size());
    for (const bropty::CommandRecord& c : s->commands()) {
        CommandInfo ci;
        ci.prompt = c.prompt;
        ci.input = c.input;
        ci.output = c.output;
        ci.end = c.end;
        ci.exitCode = c.exit_code;
        ci.finished = c.finished;
        ci.commandLine = c.command_line;  // the server read it off its screen when the shell gave none
        out.push_back(std::move(ci));
    }
    return out;
}

std::string TermSession::muxPointerShape() const {
    const bromux::ScreenModel* s = muxModel();
    return s ? s->pointer_shape() : std::string();
}

TermSession::ImageStats TermSession::muxImageStats() const {
    ImageStats st;
    st.limit = bropty::GraphicsOptions{}.storage_limit;  // the server's terminals keep bropty's quota
    const bromux::ScreenModel* s = muxModel();
    if (!s) return st;
    st.images = s->images().image_count();
    st.placements = s->images().placement_count();
    st.bytes = s->images().bytes();
    return st;
}

void TermSession::muxSetForeground(std::optional<bropty::ProcessInfo> info) {
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
}

void TermSession::muxFrameApplied(bool pointerShape, bool commands) {
    if (commands) commandsVersion_.fetch_add(1, std::memory_order_relaxed);
    if (pointerShape) {
        TermEvent e;
        e.kind = TermEvent::Kind::PointerShape;
        e.text = muxPointerShape();
        pushEvent(std::move(e));
    }
}

} // namespace bro::terminal
