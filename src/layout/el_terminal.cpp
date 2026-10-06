// ElTerminal core: the registry, the font and the grid it sizes, the child,
// keys and text, and the per-frame pump. The layer, the mouse, the view, the
// events and the theme are in their own el_terminal_*.cpp; a build without
// BRO_WITH_TERMINAL compiles el_terminal_off.cpp instead of all of them but
// this file's shared half.

#include "layout/el_terminal.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>

#if BRO_WITH_TERMINAL
#include "layout/el_terminal_impl.h"
#include "terminal/term_cwd.h"
#endif

namespace bro::layout {

// ===========================================================================
// Shared by both builds

std::vector<ElTerminal*>& termRegistry() {
    static std::vector<ElTerminal*> r;
    return r;
}

uint64_t termNextLayerId() {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

int termIntAttr(const dom::Element* el, const char* name, int fallback, int lo, int hi) {
    if (!el || !el->hasAttribute(name)) return fallback;
    const std::string& v = el->getAttribute(name);
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (end == v.c_str()) return fallback;
    return int(std::clamp<long>(n, lo, hi));
}

namespace {
ElTerminal::Host& hostSlot() {
    static ElTerminal::Host h;
    return h;
}
} // namespace

void ElTerminal::setHost(Host host) { hostSlot() = std::move(host); }
const ElTerminal::Host& ElTerminal::host() { return hostSlot(); }

void ElTerminal::forEach(const std::function<void(ElTerminal&)>& fn) {
    // A callback may create or destroy terminals (an event listener does):
    // walk a copy, and skip any that left the registry meanwhile.
    const std::vector<ElTerminal*> live = termRegistry();
    for (ElTerminal* t : live) {
        const auto& now = termRegistry();
        if (std::find(now.begin(), now.end(), t) != now.end()) fn(*t);
    }
}

ElTerminal* ElTerminal::byId(uint64_t id) {
    for (ElTerminal* t : termRegistry())
        if (t->layerId_ == id) return t;
    return nullptr;
}

#if BRO_WITH_TERMINAL

// ===========================================================================
// Compiled in

bool ElTerminal::available() { return true; }

ElTerminal::ElTerminal(render::Renderer* renderer)
    : renderer_(renderer), impl_(std::make_unique<Impl>()), layerId_(termNextLayerId()) {
    impl_->session = std::make_unique<terminal::TermSession>(80, 24);
    impl_->layer = std::make_shared<TermLayer>(layerId_);
#if defined(__linux__) || defined(__FreeBSD__)
    options_.middleClickPaste = true;  // the X11 habit
#endif
    termRegistry().push_back(this);
}

ElTerminal::~ElTerminal() {
    auto& r = termRegistry();
    r.erase(std::remove(r.begin(), r.end(), this), r.end());
    // The session's destructor stops the parser thread and the child. The
    // layer lives on while a replay holds it.
}

namespace {

// A CSS length in px: "12px", "0.1em" (of `fontSize`), a bare number.
float cssLengthPx(const std::string& v, float fontSize) {
    char* end = nullptr;
    const float n = std::strtof(v.c_str(), &end);
    if (end == v.c_str() || !std::isfinite(n)) return 0.0f;
    const std::string unit(end);
    if (unit == "em") return n * fontSize;
    if (unit == "rem") return n * 16.0f;
    return n;  // "px" or unitless
}

} // namespace

void ElTerminal::refreshFont() {
    if (!elem_) return;
    const auto& style = elem_->computedStyle();
    std::string family = "monospace";
    if (auto it = style.find("font-family"); it != style.end() && !it->second.empty()) {
        family = it->second;
        if (family.size() >= 2 && (family.front() == '"' || family.front() == '\'') && family.back() == family.front())
            family = family.substr(1, family.size() - 2);
    }
    float size = 14.0f;
    if (auto it = style.find("font-size"); it != style.end()) {
        char* end = nullptr;
        const float v = std::strtof(it->second.c_str(), &end);
        if (end != it->second.c_str() && v > 0) size = v;
    }
    int weight = 400;
    if (auto it = style.find("font-weight"); it != style.end()) {
        if (it->second == "bold" || it->second == "bolder") weight = 700;
        else if (it->second == "lighter") weight = 100;
        else {
            char* end = nullptr;
            const long v = std::strtol(it->second.c_str(), &end, 10);
            if (end != it->second.c_str() && v > 0) weight = int(v);
        }
    }
    bool italic = false;
    if (auto it = style.find("font-style"); it != style.end())
        italic = it->second == "italic" || it->second.rfind("oblique", 0) == 0;
    float lineHeight = 0.0f;  // "normal": the font's own height
    if (auto it = style.find("line-height"); it != style.end() && it->second != "normal") {
        const std::string& v = it->second;
        char* end = nullptr;
        const float n = std::strtof(v.c_str(), &end);
        if (end != v.c_str() && n > 0) {
            const std::string unit(end);
            if (unit.empty()) lineHeight = n * size;  // a multiplier
            else if (unit == "%") lineHeight = n * size / 100.0f;
            else lineHeight = cssLengthPx(v, size);
        }
    }
    float letterSpacing = 0.0f;
    if (auto it = style.find("letter-spacing"); it != style.end() && it->second != "normal")
        letterSpacing = cssLengthPx(it->second, size);

    const uint64_t gen = renderer_ ? renderer_->fontGeneration() : 0;
    std::lock_guard<std::mutex> g(impl_->fontMu);
    Impl& m = *impl_;
    const bool ligatures = options_.ligatures;
    if (m.haveMetrics && family == m.family && size == m.size && weight == m.weight && italic == m.italic &&
        lineHeight == m.lineHeight && letterSpacing == m.letterSpacing && ligatures == m.ligatures &&
        gen == m.fontGeneration && m.metrics.scale == m.scale)
        return;
    m.family = std::move(family);
    m.size = size;
    m.weight = weight;
    m.italic = italic;
    m.lineHeight = lineHeight;
    m.letterSpacing = letterSpacing;
    m.ligatures = ligatures;
    m.fontGeneration = gen;
    // Ligatures join glyphs across cells, which needs the font's own advance
    // as the cell width; otherwise the cell snaps to whole device pixels.
    m.metrics = terminal::CellMetrics::measure(renderer_, m.font(), lineHeight, letterSpacing, m.scale, ligatures);
    m.haveMetrics = true;
    m.layerDirty = true;
}

void ElTerminal::getContentSize(float& w, float& h) {
    refreshFont();
    const int cols = termIntAttr(elem_, "cols", 80, 1, 1000);
    const int rows = termIntAttr(elem_, "rows", 24, 1, 1000);
    std::lock_guard<std::mutex> g(impl_->fontMu);
    w = std::ceil(float(cols) * impl_->metrics.cellW);
    h = std::ceil(float(rows) * impl_->metrics.cellH);
}

ElTerminal::Metrics ElTerminal::metrics() const {
    const terminal::CellMetrics c = impl_->cellMetrics();
    Metrics out;
    out.cellWidth = c.cellW;
    out.cellHeight = c.cellH;
    out.baseline = c.baseline;
    out.pixelWidth = c.pixelWidth();
    out.pixelHeight = c.pixelHeight();
    out.scale = c.scale;
    return out;
}

// ---- the child ---------------------------------------------------------------

bool ElTerminal::spawn(const SpawnSpec& spec, std::string* error) {
    terminal::SpawnOptions o;
    o.command = spec.command;
    o.args = spec.args;
    o.cwd = spec.cwd;
    o.env = spec.env;
    if (!readySession(/*attaching=*/false, error)) return false;
    if (spec.persistent) {
        terminal::TermSession::PersistentOptions p;
        p.server = spec.server;
        p.name = spec.name;
        return impl_->session->spawnPersistent(o, p, error);
    }
    return impl_->session->spawn(o, error);
}

bool ElTerminal::write(std::string_view bytes) { return impl_->session->write(bytes); }
void ElTerminal::feed(std::string_view output) { impl_->session->feed(output); }
void ElTerminal::kill() { impl_->session->kill(); }
int64_t ElTerminal::pid() const { return impl_->session->pid(); }
bool ElTerminal::running() const { return impl_->session->running(); }
bool ElTerminal::exited() const { return impl_->session->exited(); }
std::optional<int> ElTerminal::exitCode() const { return impl_->session->exitCode(); }
int ElTerminal::cols() const { return impl_->session->cols(); }
int ElTerminal::rows() const { return impl_->session->rows(); }
std::string ElTerminal::screenText() const { return impl_->session->screenText(); }
std::string ElTerminal::scrollbackText() const { return impl_->session->scrollbackText(); }
std::string ElTerminal::title() const { return impl_->session->title(); }
std::string ElTerminal::cwd() const { return terminal::cwdFromUri(impl_->session->cwd()).path; }
std::string ElTerminal::cwdUri() const { return impl_->session->cwd(); }
bool ElTerminal::bracketedPaste() const { return impl_->session->modes().bracketed_paste; }
std::string ElTerminal::selectionText() const { return impl_->session->selectionText(); }
std::string ElTerminal::defaultShell() { return terminal::defaultShell(); }

std::string ElTerminal::frameText() const {
    const auto& f = impl_->frame;
    if (!f) return {};
    std::vector<std::string> lines;
    for (const auto& row : f->lines) lines.push_back(row ? row->view().text() : std::string());
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) out.push_back('\n');
        out += lines[i];
    }
    return out;
}

ElTerminal::CursorInfo ElTerminal::cursor() const {
    const bropty::CursorState c = impl_->session->cursor();
    CursorInfo out;
    out.row = c.row;
    out.col = c.col;
    out.visible = c.visible;
    out.blink = c.blink;
    out.shape = c.shape == bropty::CursorShape::Bar ? "bar"
              : c.shape == bropty::CursorShape::Underline ? "underline" : "block";
    return out;
}

// ---- input -------------------------------------------------------------------

void ElTerminal::flushDeferredKey() {
    Impl& m = *impl_;
    if (!m.haveDeferred) return;
    m.haveDeferred = false;
    m.session->sendKey(m.deferred);  // no text came: the encoder derives it
}

bool ElTerminal::keyDown(int keycode, int scancode, int sdlMod, bool repeat) {
    Impl& m = *impl_;
    flushDeferredKey();
    m.suppressText = false;
    const terminal::TranslatedKey tk = terminal::translateKey(
        keycode, scancode, sdlMod, repeat ? bropty::KeyAction::Repeat : bropty::KeyAction::Press);
    if (tk.kind == terminal::KeyKind::None) return false;
    m.blinkEpoch = m.nowMs;  // the cursor shows solid while typing
    m.layerDirty = true;
    if (tk.kind == terminal::KeyKind::Functional) {
        m.session->sendKey(tk.ev);
        m.suppressText = true;
        return true;
    }
    const bool ctrl = (tk.ev.mods & bropty::Mod_Ctrl) != 0;
    const bool alt = (tk.ev.mods & bropty::Mod_Alt) != 0;
    const bool super = (tk.ev.mods & bropty::Mod_Super) != 0;
    // Cmd/Win shortcuts belong to the application unless the program asked
    // for every key (the kitty protocol reports super).
    if (super && m.session->kittyKeyboardFlags() == 0) return false;
    if ((!ctrl && !alt && !super) || (ctrl && alt)) {
        // Typing (AltGr is Ctrl+Alt on Windows): wait for the text it makes.
        m.deferred = tk.ev;
        m.haveDeferred = true;
        return true;
    }
    m.session->sendKey(tk.ev);
    m.suppressText = true;
    return true;
}

void ElTerminal::keyCancelled() {
    flushDeferredKey();
    impl_->suppressText = true;
}

bool ElTerminal::keyUp(int keycode, int scancode, int sdlMod) {
    flushDeferredKey();
    // The text a key makes arrives between its press and its release: past
    // the release, text is typed (an IME commit), not that key's echo.
    impl_->suppressText = false;
    const terminal::TranslatedKey tk = terminal::translateKey(keycode, scancode, sdlMod, bropty::KeyAction::Release);
    if (tk.kind == terminal::KeyKind::None) return false;
    impl_->session->sendKey(tk.ev);  // only the kitty protocol (flag 2) reports releases
    return true;
}

bool ElTerminal::textInput(std::string_view text) {
    Impl& m = *impl_;
    m.blinkEpoch = m.nowMs;
    m.layerDirty = true;
    preedit_.clear();
    if (m.haveDeferred) {
        m.haveDeferred = false;
        m.deferred.text = std::string(text);
        m.session->sendKey(m.deferred);
        return true;
    }
    if (m.suppressText) {
        m.suppressText = false;
        return true;
    }
    return m.session->sendText(text);
}

void ElTerminal::setPreedit(std::string_view text) {
    // A composition starting swallows the key that started it.
    if (!text.empty()) impl_->haveDeferred = false;
    preedit_ = std::string(text);
    impl_->layerDirty = true;
}

bool ElTerminal::paste(std::string_view text) {
    flushDeferredKey();
    impl_->blinkEpoch = impl_->nowMs;
    impl_->layerDirty = true;
    return impl_->session->paste(text);
}

bool ElTerminal::caretRect(float& x, float& y, float& w, float& h) const {
    const Impl& m = *impl_;
    if (!m.frame || !elem_) return false;
    const terminal::CellMetrics metrics = m.cellMetrics();
    // Where the layer sits: document space, less the viewport scroll.
    const dom::AbsoluteRect box = dom::absoluteContentBox(elem_);
    const dom::Document* doc = elem_->document();
    const float scrollY = doc ? doc->viewportScrollY() : 0.0f;
    const int row = std::max(0, m.frame->cursor_y);
    x = box.x + float(m.frame->cursor.col) * metrics.cellW;
    y = box.y - scrollY + float(row) * metrics.cellH;
    w = metrics.cellW;
    h = metrics.cellH;
    return true;
}

// ---- the main loop -------------------------------------------------------------

bool ElTerminal::pump(double nowMs, bool focused, float scale) {
    Impl& m = *impl_;
    m.nowMs = nowMs;
    if (!elem_) return false;
    detachIfRemoved();

    if (scale > 0.0f && std::isfinite(scale)) {
        std::lock_guard<std::mutex> g(m.fontMu);
        m.scale = scale;
    }
    refreshFont();
    refreshTheme();
    const terminal::CellMetrics metrics = m.cellMetrics();

    // The grid follows the content box; the PTY's pixel size counts device px.
    const auto& box = elem_->layoutBox();
    if (box.contentRect.width > 0 && box.contentRect.height > 0 && metrics.cellW > 0 && metrics.cellH > 0) {
        const int cols = std::max(1, int(std::floor(box.contentRect.width / metrics.cellW + 1e-3f)));
        const int rows = std::max(1, int(std::floor(box.contentRect.height / metrics.cellH + 1e-3f)));
        m.session->resize(cols, rows, metrics.pixelWidth(), metrics.pixelHeight());
    }
    if (m.session->cols() != m.lastCols || m.session->rows() != m.lastRows) {
        const bool first = m.lastCols == 0;
        m.lastCols = m.session->cols();
        m.lastRows = m.session->rows();
        m.layerDirty = true;
        if (!first)
            termDispatch(elem_, "resize",
                         "{\"cols\":" + std::to_string(m.lastCols) + ",\"rows\":" + std::to_string(m.lastRows) + "}");
    }

    if (focused != m.focused) {
        m.focused = focused;
        if (!focused) {
            flushDeferredKey();
            preedit_.clear();
        }
        m.session->focus(focused);  // reported only under ?1004
        m.blinkEpoch = nowMs;
        m.layerDirty = true;
    }

    if (m.session->hasNewFrame() || !m.frame) {
        m.frame = m.session->acquireFrame();
        m.layerDirty = true;
    }

    // Blink: 530 ms on, 530 off, restarted by input.
    bool blinkOn = true;
    if (m.focused && m.frame && (m.frame->cursor.blink || m.frame->modes.cursor_blink))
        blinkOn = std::fmod(std::max(0.0, nowMs - m.blinkEpoch), 1060.0) < 530.0;
    if (blinkOn != m.blinkOn) {
        m.blinkOn = blinkOn;
        m.layerDirty = true;
    }

    autoScroll(nowMs);
    dispatchEvents();
    dispatchActivity();

    if (m.session->exited() && !m.exitDispatched) {
        m.exitDispatched = true;
        if (m.session->hasNewFrame()) m.frame = m.session->acquireFrame();
        const auto code = m.session->exitCode();
        termDispatch(elem_, "exit", code ? "{\"exitCode\":" + std::to_string(*code) + "}" : "{\"exitCode\":null}");
        m.layerDirty = true;
    } else if (m.session->detached() && !m.session->exited() && !m.detachDispatched) {
        m.detachDispatched = true;
        termDispatch(elem_, "detach", "{\"sessionId\":" + std::to_string(m.session->sessionId()) + "}");
    }
    return m.layerDirty.load();
}

#endif  // BRO_WITH_TERMINAL

} // namespace bro::layout
