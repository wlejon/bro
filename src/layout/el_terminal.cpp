#include "layout/el_terminal.h"

#include "dom/element.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>

#if BRO_WITH_TERMINAL
#include "terminal/term_keys.h"
#include "terminal/term_paint.h"
#include "terminal/term_session.h"
#endif

namespace bro::layout {

namespace {

std::vector<ElTerminal*>& registry() {
    static std::vector<ElTerminal*> r;
    return r;
}

int intAttr(const dom::Element* el, const char* name, int fallback, int lo, int hi) {
    if (!el || !el->hasAttribute(name)) return fallback;
    const std::string& v = el->getAttribute(name);
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (end == v.c_str()) return fallback;
    return int(std::clamp<long>(n, lo, hi));
}

} // namespace

void ElTerminal::forEach(const std::function<void(ElTerminal&)>& fn) {
    // A callback may create or destroy terminals (an event listener does):
    // walk a copy, and skip any that left the registry meanwhile.
    const std::vector<ElTerminal*> live = registry();
    for (ElTerminal* t : live) {
        const auto& now = registry();
        if (std::find(now.begin(), now.end(), t) != now.end()) fn(*t);
    }
}

#if BRO_WITH_TERMINAL

// ===========================================================================
// Compiled in

struct ElTerminal::Impl {
    std::unique_ptr<terminal::TermSession> session;
    terminal::TermPainter painter;
    std::shared_ptr<const bropty::Frame> frame;  // the frame drawn (main thread)

    // The element's font and the cell metrics measured from it. Read by
    // getContentSize() on the layout thread too, hence the lock.
    mutable std::mutex fontMu;
    std::string family = "monospace";
    float size = 14.0f;
    int weight = 400;
    bool italic = false;
    float lineHeight = 0.0f;
    uint64_t fontGeneration = ~0ull;
    terminal::CellMetrics metrics;
    bool haveMetrics = false;

    // Focus, blink, events.
    bool focused = false;
    bool blinkOn = true;
    double nowMs = 0;
    double blinkEpoch = 0;
    bool exitDispatched = false;
    int lastCols = 0, lastRows = 0;

    // A text key held until the text input it produces arrives (or the key
    // is released, or another key comes first), so the encoder gets both the
    // key and the text it typed (kitty's associated text, keypad vs digits).
    bool haveDeferred = false;
    bropty::KeyEvent deferred;
    // A key sent on its own (Ctrl+C, Enter, keypad): the text input SDL
    // generates for it, if any, is not typed a second time.
    bool suppressText = false;

    // Where the content box was last drawn, for the IME window.
    bool drawn = false;
    float drawX = 0, drawY = 0;

    render::FontRef font() const { return render::FontRef{family, size, weight, italic}; }
};

bool ElTerminal::available() { return true; }

ElTerminal::ElTerminal(render::Renderer* renderer) : renderer_(renderer), impl_(std::make_unique<Impl>()) {
    impl_->session = std::make_unique<terminal::TermSession>(80, 24);
    registry().push_back(this);
}

ElTerminal::~ElTerminal() {
    auto& r = registry();
    r.erase(std::remove(r.begin(), r.end(), this), r.end());
    // The session's destructor stops the parser thread and the child.
}

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
        if (it->second == "bold") weight = 700;
        else if (it->second == "lighter") weight = 100;
        else if (it->second == "bolder") weight = 700;
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
            if (unit.empty()) lineHeight = n * size;           // a multiplier
            else if (unit == "px") lineHeight = n;
            else if (unit == "%") lineHeight = n * size / 100.0f;
            else if (unit == "em") lineHeight = n * size;
        }
    }

    const uint64_t gen = renderer_ ? renderer_->fontGeneration() : 0;
    std::lock_guard<std::mutex> g(impl_->fontMu);
    Impl& m = *impl_;
    if (m.haveMetrics && family == m.family && size == m.size && weight == m.weight && italic == m.italic &&
        lineHeight == m.lineHeight && gen == m.fontGeneration)
        return;
    m.family = std::move(family);
    m.size = size;
    m.weight = weight;
    m.italic = italic;
    m.lineHeight = lineHeight;
    m.fontGeneration = gen;
    m.metrics = terminal::CellMetrics::measure(renderer_, m.font(), lineHeight);
    m.haveMetrics = true;
}

void ElTerminal::getContentSize(float& w, float& h) {
    refreshFont();
    const int cols = intAttr(elem_, "cols", 80, 1, 1000);
    const int rows = intAttr(elem_, "rows", 24, 1, 1000);
    std::lock_guard<std::mutex> g(impl_->fontMu);
    w = std::ceil(float(cols) * impl_->metrics.cellW);
    h = std::ceil(float(rows) * impl_->metrics.cellH);
}

void ElTerminal::draw(render::Renderer* renderer, const htmlayout::layout::LayoutBox& box, float offsetX,
                      float offsetY) {
    Impl& m = *impl_;
    if (!m.frame) m.frame = m.session->acquireFrame();
    if (!m.frame || !renderer) return;
    const float x = box.contentRect.x + offsetX;
    const float y = box.contentRect.y + offsetY;
    const float w = box.contentRect.width;
    const float h = box.contentRect.height;
    if (w <= 0 || h <= 0) return;
    m.drawn = true;
    m.drawX = x;
    m.drawY = y;
    terminal::CellMetrics metrics;
    terminal::PaintOptions opts;
    {
        std::lock_guard<std::mutex> g(m.fontMu);
        metrics = m.metrics;
    }
    opts.font = m.font();  // family views m.family, stable for the paint
    opts.focused = m.focused;
    opts.blinkOn = m.blinkOn;
    opts.preedit = preedit_;
    m.painter.paint(renderer, *m.frame, x, y, w, h, metrics, opts);
}

// ---- the child ---------------------------------------------------------------

bool ElTerminal::spawn(const SpawnSpec& spec, std::string* error) {
    terminal::SpawnOptions o;
    o.command = spec.command;
    o.args = spec.args;
    o.cwd = spec.cwd;
    o.env = spec.env;
    impl_->exitDispatched = false;
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
}

bool ElTerminal::paste(std::string_view text) {
    flushDeferredKey();
    impl_->blinkEpoch = impl_->nowMs;
    return impl_->session->paste(text);
}

bool ElTerminal::caretRect(float& x, float& y, float& w, float& h) const {
    const Impl& m = *impl_;
    if (!m.drawn || !m.frame) return false;
    terminal::CellMetrics metrics;
    {
        std::lock_guard<std::mutex> g(m.fontMu);
        metrics = m.metrics;
    }
    const int row = std::max(0, m.frame->cursor_y);
    x = m.drawX + float(m.frame->cursor.col) * metrics.cellW;
    y = m.drawY + float(row) * metrics.cellH;
    w = metrics.cellW;
    h = metrics.cellH;
    return true;
}

// ---- the main loop -------------------------------------------------------------

bool ElTerminal::pump(double nowMs, bool focused) {
    Impl& m = *impl_;
    m.nowMs = nowMs;
    bool repaint = false;
    if (!elem_) return false;

    refreshFont();
    terminal::CellMetrics metrics;
    {
        std::lock_guard<std::mutex> g(m.fontMu);
        metrics = m.metrics;
    }

    // The grid follows the content box.
    const auto& box = elem_->layoutBox();
    if (box.contentRect.width > 0 && box.contentRect.height > 0 && metrics.cellW > 0 && metrics.cellH > 0) {
        const int cols = std::max(1, int(std::floor(box.contentRect.width / metrics.cellW + 1e-3f)));
        const int rows = std::max(1, int(std::floor(box.contentRect.height / metrics.cellH + 1e-3f)));
        m.session->resize(cols, rows, int(std::lround(metrics.cellW)), int(std::lround(metrics.cellH)));
    }
    if (m.session->cols() != m.lastCols || m.session->rows() != m.lastRows) {
        const bool first = m.lastCols == 0;
        m.lastCols = m.session->cols();
        m.lastRows = m.session->rows();
        repaint = true;
        if (!first) {
            dom::CustomEvent evt("resize", false, false);
            evt.setDetail("{\"cols\":" + std::to_string(m.lastCols) + ",\"rows\":" + std::to_string(m.lastRows) + "}");
            evt.setIsTrusted(true);
            dom::dispatchDomEvent(elem_, evt);
        }
    }

    if (focused != m.focused) {
        m.focused = focused;
        if (!focused) {
            flushDeferredKey();
            preedit_.clear();
        }
        m.session->focus(focused);  // reported only under ?1004
        m.blinkEpoch = nowMs;
        repaint = true;
    }

    if (m.session->hasNewFrame() || !m.frame) {
        m.frame = m.session->acquireFrame();
        repaint = true;
    }

    // Blink: 530 ms on, 530 off, restarted by input.
    bool blinkOn = true;
    if (m.focused && m.frame && (m.frame->cursor.blink || m.frame->modes.cursor_blink))
        blinkOn = std::fmod(std::max(0.0, nowMs - m.blinkEpoch), 1060.0) < 530.0;
    if (blinkOn != m.blinkOn) {
        m.blinkOn = blinkOn;
        repaint = true;
    }

    if (m.session->exited() && !m.exitDispatched) {
        m.exitDispatched = true;
        if (m.session->hasNewFrame()) m.frame = m.session->acquireFrame();
        dom::CustomEvent evt("exit", false, false);
        const auto code = m.session->exitCode();
        evt.setDetail(code ? "{\"exitCode\":" + std::to_string(*code) + "}" : std::string("{\"exitCode\":null}"));
        evt.setIsTrusted(true);
        dom::dispatchDomEvent(elem_, evt);
        repaint = true;
    }
    return repaint;
}

#else

// ===========================================================================
// Compiled out: an inert box of the default size.

struct ElTerminal::Impl {};

bool ElTerminal::available() { return false; }
ElTerminal::ElTerminal(render::Renderer* renderer) : renderer_(renderer) { registry().push_back(this); }
ElTerminal::~ElTerminal() {
    auto& r = registry();
    r.erase(std::remove(r.begin(), r.end(), this), r.end());
}
void ElTerminal::refreshFont() {}
void ElTerminal::getContentSize(float& w, float& h) {
    w = float(intAttr(elem_, "cols", 80, 1, 1000)) * 8.0f;
    h = float(intAttr(elem_, "rows", 24, 1, 1000)) * 16.0f;
}
void ElTerminal::draw(render::Renderer*, const htmlayout::layout::LayoutBox&, float, float) {}
bool ElTerminal::spawn(const SpawnSpec&, std::string* error) {
    if (error) *error = "<terminal> is not compiled into this build (BRO_WITH_TERMINAL)";
    return false;
}
bool ElTerminal::write(std::string_view) { return false; }
void ElTerminal::feed(std::string_view) {}
void ElTerminal::kill() {}
int64_t ElTerminal::pid() const { return 0; }
bool ElTerminal::running() const { return false; }
bool ElTerminal::exited() const { return false; }
std::optional<int> ElTerminal::exitCode() const { return std::nullopt; }
int ElTerminal::cols() const { return 0; }
int ElTerminal::rows() const { return 0; }
std::string ElTerminal::screenText() const { return {}; }
std::string ElTerminal::scrollbackText() const { return {}; }
std::string ElTerminal::frameText() const { return {}; }
std::string ElTerminal::title() const { return {}; }
std::string ElTerminal::selectionText() const { return {}; }
std::string ElTerminal::defaultShell() { return {}; }
ElTerminal::CursorInfo ElTerminal::cursor() const { return {}; }
void ElTerminal::flushDeferredKey() {}
bool ElTerminal::keyDown(int, int, int, bool) { return false; }
bool ElTerminal::keyUp(int, int, int) { return false; }
void ElTerminal::keyCancelled() {}
bool ElTerminal::textInput(std::string_view) { return false; }
void ElTerminal::setPreedit(std::string_view) {}
bool ElTerminal::paste(std::string_view) { return false; }
bool ElTerminal::caretRect(float&, float&, float&, float&) const { return false; }
bool ElTerminal::pump(double, bool) { return false; }

#endif

} // namespace bro::layout
