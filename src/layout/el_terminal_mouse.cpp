// ElTerminal's mouse: selection gestures, reports to the program, the wheel,
// links under the pointer and the pointer's shape.
//
// Who gets a press: a program that turned mouse reporting on (?9 / ?1000 /
// ?1002 / ?1003) gets it, encoded for its mode (?1006 SGR, ?1016 SGR-pixels,
// ...), unless Shift is held: Shift always selects, as in xterm. Otherwise a
// left press selects (a click by character, a double click by word, a triple
// click by line, Alt for a block), Shift+click extends the selection,
// Ctrl+click (Cmd+click on macOS) on a link raises `linkactivate`, and a
// middle click pastes when middleClickPaste is on. The wheel scrolls the
// view through history unless the program is capturing the mouse, when it
// is reported (or, on the alternate screen under ?1007, sent as arrow keys).

#include "layout/el_terminal_impl.h"
#include "terminal/term_cwd.h"

#include "dom/element.h"
#include "dom/element_geometry.h"

#include <algorithm>
#include <cmath>

namespace bro::layout {

namespace {

constexpr int kModShift = 0x0001 | 0x0002;  // platform::kmod::LShift | RShift
constexpr int kModCtrl = 0x0040 | 0x0080;
constexpr int kModAlt = 0x0100 | 0x0200;
constexpr int kModGui = 0x0400 | 0x0800;

bool linkChord(int mod) {
#ifdef __APPLE__
    return (mod & kModGui) != 0;
#else
    return (mod & kModCtrl) != 0;
#endif
}

bropty::MouseButton toButton(int domButton) {
    switch (domButton) {
        case 0: return bropty::MouseButton::Left;
        case 1: return bropty::MouseButton::Middle;
        case 2: return bropty::MouseButton::Right;
        case 3: return bropty::MouseButton::Button8;
        case 4: return bropty::MouseButton::Button9;
        default: return bropty::MouseButton::None;
    }
}

// Where a document-space point falls on the grid.
struct GridHit {
    bropty::RowPos cell;    // absolute row, column (clamped to the grid)
    bool rightHalf = false;
    int viewRow = 0;        // row on screen, clamped
    int px = 0, py = 0;     // device pixels from the grid's origin, clamped
    int above = 0;          // rows the point is above (< 0) / below (> 0) the grid
};

} // namespace

namespace {

GridHit gridHit(const ElTerminal::Impl& m, const dom::Element* el, float docX, float docY) {
    GridHit h;
    const terminal::CellMetrics cm = m.cellMetrics();
    const dom::AbsoluteRect box = dom::absoluteContentBox(el);
    const int cols = std::max(1, m.session->cols());
    const int rows = std::max(1, m.session->rows());
    const float lx = docX - box.x, ly = docY - box.y;
    const float fx = cm.cellW > 0 ? lx / cm.cellW : 0.0f;
    const float fy = cm.cellH > 0 ? ly / cm.cellH : 0.0f;
    const int col = int(std::floor(fx));
    const int row = int(std::floor(fy));
    h.above = row < 0 ? row : row >= rows ? row - rows + 1 : 0;
    h.viewRow = std::clamp(row, 0, rows - 1);
    if (col < 0) {
        h.cell.col = 0;
    } else if (col >= cols) {
        h.cell.col = cols - 1;
        h.rightHalf = true;
    } else {
        h.cell.col = col;
        h.rightHalf = fx - float(col) >= 0.5f;
    }
    if (row < 0) {
        h.cell.col = 0;
        h.rightHalf = false;
    } else if (row >= rows) {
        h.cell.col = cols - 1;
        h.rightHalf = true;
    }
    h.cell.row = m.session->viewState().topRow + h.viewRow;
    const float s = cm.scale > 0 ? cm.scale : 1.0f;
    h.px = std::clamp(int(std::floor(lx * s)), 0, std::max(0, int(std::lround(float(cols) * cm.cellW * s)) - 1));
    h.py = std::clamp(int(std::floor(ly * s)), 0, std::max(0, int(std::lround(float(rows) * cm.cellH * s)) - 1));
    return h;
}

bropty::MouseEvent mouseEvent(bropty::MouseAction action, bropty::MouseButton button, int sdlMod,
                              const GridHit& h) {
    bropty::MouseEvent ev;
    ev.action = action;
    ev.button = button;
    ev.mods = terminal::translateMods(sdlMod);
    ev.col = h.cell.col;
    ev.row = h.viewRow;
    ev.x = h.px;
    ev.y = h.py;
    return ev;
}

// What the OS opens for a link: a URI as it is; a path as a file:// URL, a
// relative one resolved against the shell's directory (OSC 7) when known.
std::string openTarget(const terminal::LinkInfo& link, const std::string& cwdUri) {
    if (link.kind != "path") return link.target;
    std::string p = link.target;
    for (char& c : p)
        if (c == '\\') c = '/';
    const bool absolute = (!p.empty() && p[0] == '/') || (p.size() > 1 && p[1] == ':');
    if (!absolute) {
        // file://host/dir -> /dir (decoded, as `cwd` reads it)
        std::string dir = terminal::cwdFromUri(cwdUri).path;
        for (char& c : dir)
            if (c == '\\') c = '/';
        if (dir.empty()) return link.target;
        if (p.rfind("./", 0) == 0) p = p.substr(2);
        if (p.rfind("~/", 0) == 0) return link.target;
        p = dir + (dir.back() == '/' ? "" : "/") + p;
    }
    if (p.size() > 1 && p[1] == ':') p = "/" + p;  // file:///C:/...
    return "file://" + p;
}

bool reportingMouse(const ElTerminal::Impl& m, int sdlMod) {
    return m.session->mouseTracking() != bropty::MouseTracking::None && !(sdlMod & kModShift);
}

} // namespace

ElTerminal::MouseResult ElTerminal::mouseDown(float docX, float docY, int button, int sdlMod, int clicks) {
    Impl& m = *impl_;
    MouseResult res;
    if (!elem_) return res;
    m.lastDocX = docX;
    m.lastDocY = docY;
    m.lastMod = sdlMod;
    const GridHit h = gridHit(m, elem_, docX, docY);

    if (reportingMouse(m, sdlMod)) {
        const bropty::MouseButton b = toButton(button);
        if (b == bropty::MouseButton::None) return res;
        m.session->sendMouse(mouseEvent(bropty::MouseAction::Press, b, sdlMod, h));
        m.reportHeld |= 1 << button;
        res.handled = true;
        res.capture = true;
        return res;
    }

    if (button == 1) {
        if (!options_.middleClickPaste) return res;
        const Host& host = ElTerminal::host();
        const bool primary =
#if defined(__linux__) || defined(__FreeBSD__)
            true;
#else
            false;
#endif
        res.pasteText = host.readClipboard ? host.readClipboard(primary) : std::string();
        res.handled = true;
        return res;
    }
    if (button != 0) return res;

    if (linkChord(sdlMod)) {
        if (auto link = m.session->linkAt(h.cell)) {
            const std::string detail = "{\"uri\":" + termJsonString(link->target) + ",\"kind\":" +
                                       termJsonString(link->kind) + ",\"text\":" + termJsonString(link->text) + "}";
            if (termDispatch(elem_, "linkactivate", detail, /*cancelable=*/true)) {
                const Host& host = ElTerminal::host();
                if (host.openLink) host.openLink(openTarget(*link, m.session->cwd()), link->kind);
            }
            res.handled = true;
            return res;
        }
    }

    m.drag = Impl::Drag{};
    m.drag.selecting = true;
    m.drag.anchor = h.cell;
    if ((sdlMod & kModShift) && m.session->selectionActive()) {
        m.drag.extend = true;
        m.session->selectExtend(h.cell, h.rightHalf);
    } else {
        const bropty::SelectionMode mode = clicks >= 3 ? bropty::SelectionMode::Line
                                         : clicks == 2 ? bropty::SelectionMode::Word
                                         : (sdlMod & kModAlt) ? bropty::SelectionMode::Block
                                         : bropty::SelectionMode::Character;
        m.drag.character = mode == bropty::SelectionMode::Character || mode == bropty::SelectionMode::Block;
        m.session->selectStart(h.cell, mode, h.rightHalf);
    }
    res.handled = true;
    res.capture = true;
    return res;
}

ElTerminal::MouseResult ElTerminal::mouseMove(float docX, float docY, int sdlMod) {
    Impl& m = *impl_;
    MouseResult res;
    if (!elem_) return res;
    m.lastDocX = docX;
    m.lastDocY = docY;
    m.lastMod = sdlMod;
    m.pointerInside = true;
    const GridHit h = gridHit(m, elem_, docX, docY);

    if (m.drag.selecting) {
        if (h.cell != m.drag.anchor || m.drag.extend) m.drag.moved = true;
        if (m.drag.moved) m.session->selectExtend(h.cell, h.rightHalf);
        res.handled = true;
        res.capture = true;
        return res;
    }
    if (m.reportHeld || reportingMouse(m, sdlMod)) {
        // Motion: reported under ?1002 with a button held and always under
        // ?1003 (the session's reporter drops what the mode does not ask for
        // and motion that stays in its cell).
        if (h.above == 0 || m.reportHeld)
            m.session->sendMouse(mouseEvent(bropty::MouseAction::Motion, bropty::MouseButton::None, sdlMod, h));
        res.handled = true;
        res.capture = m.reportHeld != 0;
    }
    // Links under the pointer are underlined while it is there.
    std::optional<terminal::LinkInfo> link;
    if (h.above == 0 && !reportingMouse(m, sdlMod)) link = m.session->linkAt(h.cell);
    m.session->setHover(link ? std::optional<bropty::RowPos>(h.cell) : std::nullopt);
    m.hover = std::move(link);
    return res;
}

ElTerminal::MouseResult ElTerminal::mouseUp(float docX, float docY, int button, int sdlMod) {
    Impl& m = *impl_;
    MouseResult res;
    if (!elem_) return res;
    const GridHit h = gridHit(m, elem_, docX, docY);
    if (m.reportHeld & (1 << button)) {
        m.reportHeld &= ~(1 << button);
        const bropty::MouseButton b = toButton(button);
        if (b != bropty::MouseButton::None)
            m.session->sendMouse(mouseEvent(bropty::MouseAction::Release, b, sdlMod, h));
        res.handled = true;
        return res;
    }
    if (button != 0 || !m.drag.selecting) return res;
    m.drag.selecting = false;
    res.handled = true;
    if (m.drag.character && !m.drag.moved) {
        // A click: no selection.
        m.session->selectClear();
        return res;
    }
    const std::string text = m.session->selectionText();
    if (text.empty()) return res;
    const Host& host = ElTerminal::host();
    if (!host.writeClipboard) return res;
    if (options_.copyOnSelect) host.writeClipboard(text, false);
#if defined(__linux__) || defined(__FreeBSD__)
    host.writeClipboard(text, true);  // the primary selection, X11's convention
#endif
    return res;
}

bool ElTerminal::wheel(float docX, float docY, float notches, int sdlMod) {
    Impl& m = *impl_;
    if (!elem_ || notches == 0.0f) return false;
    // Whole notches; a trackpad's fractions accumulate.
    m.wheelCarry += notches;
    const int steps = int(m.wheelCarry);
    if (steps == 0) return true;
    m.wheelCarry -= float(steps);
    const GridHit h = gridHit(m, elem_, docX, docY);
    const bropty::MouseButton b = steps > 0 ? bropty::MouseButton::WheelUp : bropty::MouseButton::WheelDown;

    if (reportingMouse(m, sdlMod) || m.session->altScreen()) {
        // Reported to the program; on the alternate screen without reporting
        // the session sends arrow keys under ?1007, and nothing else scrolls
        // (that screen has no history).
        for (int i = 0; i < std::abs(steps); ++i)
            m.session->sendMouse(mouseEvent(bropty::MouseAction::Press, b, sdlMod, h));
        return true;
    }
    const terminal::ViewState before = m.session->viewState();
    m.session->scrollBy(-int64_t(steps) * std::max(1, options_.wheelLines));
    const terminal::ViewState after = m.session->viewState();
    if (after.topRow == before.topRow) {
        m.wheelCarry = 0.0f;
        return false;  // at the end of history: the page may scroll instead
    }
    return true;
}

void ElTerminal::mouseLeave() {
    Impl& m = *impl_;
    m.pointerInside = false;
    if (m.hover) {
        m.hover.reset();
        m.session->setHover(std::nullopt);
    }
}

std::string ElTerminal::pointerCursor(int sdlMod) const {
    const Impl& m = *impl_;
    if (reportingMouse(m, sdlMod)) return "default";
    std::string shape = m.session->pointerShape();
    if (!shape.empty()) return shape;
    if (m.hover) return "pointer";
    return "text";
}

// While a selection drag is outside the grid, the view scrolls towards the
// pointer (faster the further it is) and the selection follows.
void ElTerminal::autoScroll(double nowMs) {
    Impl& m = *impl_;
    if (!m.drag.selecting || !elem_) return;
    const GridHit h = gridHit(m, elem_, m.lastDocX, m.lastDocY);
    if (h.above == 0) return;
    const double interval = std::max(15.0, 80.0 - 15.0 * std::abs(h.above));
    if (nowMs - m.lastAutoScrollMs < interval) return;
    m.lastAutoScrollMs = nowMs;
    m.session->scrollBy(h.above < 0 ? -1 : 1);
    const GridHit after = gridHit(m, elem_, m.lastDocX, m.lastDocY);
    m.drag.moved = true;
    m.session->selectExtend(after.cell, after.rightHalf);
}

} // namespace bro::layout
