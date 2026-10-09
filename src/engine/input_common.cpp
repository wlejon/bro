#include "engine/input_common.h"
#include "engine/engine.h"
#include "engine/key_mapping.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/range.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "css/cascade.h"
#include "platform/keyboard.h"
#include "platform/keys.h"
#include <algorithm>
#include <cctype>

namespace bro::engine {

void safeStartTextInput(platform::Window* window) {
    if (window) window->textInput().start();
}

void safeStopTextInput(platform::Window* window) {
    if (window) window->textInput().stop();
}

int modifierBitForKeycode(int keycode) {
    switch (keycode) {
        case platform::kc::LShift: case platform::kc::RShift: return platform::kmod::Shift;
        case platform::kc::LCtrl:  case platform::kc::RCtrl:  return platform::kmod::Ctrl;
        case platform::kc::LAlt:   case platform::kc::RAlt:   return platform::kmod::Alt;
        case platform::kc::LGui:   case platform::kc::RGui:   return platform::kmod::Gui;
        default: return 0;
    }
}

bool isCaretControl(dom::Element* el) {
    if (getElTextarea(el)) return true;
    if (auto* in = getElInput(el)) return in->isTextType(el);
    return false;
}

dom::KeyboardEvent makeKeyboardEvent(const char* type,
                                     int keycode, int scancode,
                                     int mod, bool repeat) {
    dom::KeyboardEvent evt(type);
    evt.setKey(sdlKeycodeToWebKey(keycode, mod));
    evt.setCode(sdlScancodeToWebCode(scancode));
    evt.setCtrlKey((mod & platform::kmod::Ctrl) != 0);
    evt.setShiftKey((mod & platform::kmod::Shift) != 0);
    evt.setAltKey((mod & platform::kmod::Alt) != 0);
    evt.setMetaKey((mod & platform::kmod::Gui) != 0);
    evt.setRepeat(repeat);
    evt.setIsTrusted(true);

    if (scancode == platform::sc::LShift || scancode == platform::sc::LCtrl ||
        scancode == platform::sc::LAlt || scancode == platform::sc::LGui)
        evt.setLocation(1);
    else if (scancode == platform::sc::RShift || scancode == platform::sc::RCtrl ||
             scancode == platform::sc::RAlt || scancode == platform::sc::RGui)
        evt.setLocation(2);
    else if (keycode >= platform::kc::KpDivide && keycode <= platform::kc::KpEquals)
        evt.setLocation(3);

    return evt;
}

int sdlToDomButton(int sdlButton) {
    switch (sdlButton) {
        case 1: return 0;
        case 2: return 1;
        case 3: return 2;
        case 4: return 3;
        case 5: return 4;
        default: return sdlButton - 1;
    }
}

int domButtonMask(int domButton) {
    switch (domButton) {
        case 0: return 1;
        case 1: return 4;
        case 2: return 2;
        case 3: return 8;
        case 4: return 16;
        default: return 0;
    }
}

void populateMouseEvent(dom::MouseEvent& evt, float x, float y,
                        int button, int buttons,
                        float movementX, float movementY,
                        float scrollY, int mod,
                        float contentTop) {
    float cy = y - contentTop;
    evt.setClientX(static_cast<double>(x));
    evt.setClientY(static_cast<double>(cy));
    evt.setScreenX(static_cast<double>(x));
    evt.setScreenY(static_cast<double>(y));
    evt.setPageX(static_cast<double>(x));
    evt.setPageY(static_cast<double>(cy + scrollY));
    evt.setMovementX(static_cast<double>(movementX));
    evt.setMovementY(static_cast<double>(movementY));
    evt.setButton(button);
    evt.setButtons(buttons);
    evt.setCtrlKey((mod & platform::kmod::Ctrl) != 0);
    evt.setShiftKey((mod & platform::kmod::Shift) != 0);
    evt.setAltKey((mod & platform::kmod::Alt) != 0);
    evt.setMetaKey((mod & platform::kmod::Gui) != 0);
    evt.setIsTrusted(true);
}

platform::CursorShape cursorShapeFromCss(const std::string& value) {
    using platform::CursorShape;
    size_t comma = value.find_last_of(',');
    std::string v = (comma == std::string::npos) ? value : value.substr(comma + 1);
    size_t b = v.find_first_not_of(" \t\r\n");
    size_t e = v.find_last_not_of(" \t\r\n");
    v = (b == std::string::npos) ? std::string() : v.substr(b, e - b + 1);
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (v.empty() || v == "auto" || v == "default") return CursorShape::Default;
    if (v == "pointer" || v == "hand")              return CursorShape::Pointer;
    if (v == "text" || v == "vertical-text")        return CursorShape::Text;
    if (v == "move" || v == "grab" || v == "grabbing" || v == "all-scroll")
        return CursorShape::Move;
    if (v == "crosshair" || v == "cell")            return CursorShape::Crosshair;
    if (v == "wait")                                return CursorShape::Wait;
    if (v == "progress")                            return CursorShape::Progress;
    if (v == "not-allowed" || v == "no-drop")       return CursorShape::NotAllowed;
    if (v == "ew-resize" || v == "e-resize" || v == "w-resize" || v == "col-resize")
        return CursorShape::ResizeEW;
    if (v == "ns-resize" || v == "n-resize" || v == "s-resize" || v == "row-resize")
        return CursorShape::ResizeNS;
    if (v == "nesw-resize" || v == "ne-resize" || v == "sw-resize")
        return CursorShape::ResizeNESW;
    if (v == "nwse-resize" || v == "nw-resize" || v == "se-resize")
        return CursorShape::ResizeNWSE;
    if (v == "none")                                return CursorShape::None;
    return CursorShape::Default;
}

const char* cursorShapeName(platform::CursorShape s) {
    using platform::CursorShape;
    switch (s) {
        case CursorShape::Default:    return "default";
        case CursorShape::Pointer:    return "pointer";
        case CursorShape::Text:       return "text";
        case CursorShape::Move:       return "move";
        case CursorShape::Crosshair:  return "crosshair";
        case CursorShape::Wait:       return "wait";
        case CursorShape::Progress:   return "progress";
        case CursorShape::NotAllowed: return "not-allowed";
        case CursorShape::ResizeEW:   return "ew-resize";
        case CursorShape::ResizeNS:   return "ns-resize";
        case CursorShape::ResizeNESW: return "nesw-resize";
        case CursorShape::ResizeNWSE: return "nwse-resize";
        case CursorShape::None:       return "none";
        case CursorShape::Count_:     break;
    }
    return "default";
}

bool isControlChar(const std::string& text) {
    if (text.empty()) return true;
    for (unsigned char c : text) {
        if (c < 32 && c != '\t' && c != '\n' && c != '\r') return true;
        if (c == 127) return true;
    }
    return false;
}

dom::Element* editableHostOf(dom::Node* node) {
    for (dom::Node* n = node; n; n = n->parentNode()) {
        if (n->nodeType() != dom::NodeType::Element) continue;
        auto* el = static_cast<dom::Element*>(n);
        if (!el->hasAttribute("contenteditable")) continue;
        return el->getAttribute("contenteditable") != "false" ? el : nullptr;
    }
    return nullptr;
}

bool inEditableHost(dom::Node* node) {
    return editableHostOf(node) != nullptr;
}

bool isSelectionSuppressed(dom::Element* el) {
    for (auto* cur = el; cur; cur = cur->parentElement()) {
        const auto& style = cur->computedStyle();
        auto it = style.find("user-select");
        if (it == style.end()) continue;
        const std::string& v = it->second;
        if (v == "none") return true;
        if (v == "auto") continue;
        return false;
    }
    return false;
}

void markHoverChainDirty(const htmlayout::css::Cascade& cascade,
                         dom::Element* prev, dom::Element* target) {
    const bool siblingScope = cascade.hoverAffectsSiblings();
    auto isAncestorOrSelf = [](dom::Element* a, dom::Element* d) {
        for (auto* e = d; e; e = e->parentElement())
            if (e == a) return true;
        return false;
    };
    auto flipped = [&](dom::Element* e) {
        e->markStyleDirty();
        if (!cascade.hoverInvalidatesDescendants(e->tagName(), e->getAttribute("id"),
                                                 e->getAttribute("class")))
            return;
        e->markHoverScopeDirty();
        if (siblingScope && e->parentElement())
            e->parentElement()->markHoverScopeDirty();
    };
    dom::Element* lca = nullptr;
    for (auto* e = target; e; e = e->parentElement())
        if (isAncestorOrSelf(e, prev)) { lca = e; break; }
    for (auto* e = target; e && e != lca; e = e->parentElement()) flipped(e);
    for (auto* e = prev; e && e != lca; e = e->parentElement()) flipped(e);
}

void deleteRangeContents(dom::Document* doc,
                         dom::Range& r,
                         dom::Node*& node, int& off) {
    r.deleteContents();
    node = r.startContainer();
    off = r.startOffset();
    if (doc) doc->markDirty();
}

int Engine::currentModState() const {
    return (window_ ? static_cast<int>(platform::keyboard().modState()) : 0) | heldModifierMask_;
}

} // namespace bro::engine
