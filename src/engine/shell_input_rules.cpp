// What a desktop shell's DOM claims when bro hosts it as the display server
// (DRM mode): the questions the DRM input router asks before it hands a
// pointer or key event to the shell document rather than a client window.
// Both are answered from generic markup and style, with no knowledge of any
// particular shell:
//
//   pointer   the element under the point, or an ancestor of it, has a
//             computed z-index >= kShellOverlayZ. The compositor draws client
//             windows between the shell's desktop level (z-index below that:
//             wallpaper, desktop icons) and its overlays (bars, docks, menus,
//             popovers). Elements with pointer-events: none are never hit.
//
//   keyboard  a rendered, focused editable (a text input, textarea, select,
//             or inside a contenteditable host), or any rendered element carrying the
//             data-shell-keyboard attribute (an open launcher, a menu, a lock
//             screen). Rendered means: neither it nor an ancestor has the
//             `hidden` attribute or a computed display: none, and its own
//             computed visibility is not hidden or collapse.
//             data-shell-keyboard="false" opts an element out.
//
// Headless exposes both (shellClaimsPointerAt / shellClaimsKeyboard) so the
// rules are testable without a DRM session.
#include "engine/engine.h"

#include "dom/document.h"
#include "dom/element.h"

#include <cctype>
#include <string>

namespace bro::engine {

namespace {

constexpr int kShellOverlayZ = 1000;

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const std::string* styleValue(dom::Element* el, const char* prop) {
    const auto& style = el->computedStyle();
    auto it = style.find(prop);
    return it == style.end() ? nullptr : &it->second;
}

bool prunesSubtree(dom::Element* el) {
    if (el->hasAttribute("hidden")) return true;
    const std::string* d = styleValue(el, "display");
    return d && *d == "none";
}

bool claimsKeyboard(dom::Element* el) {
    if (!el->hasAttribute("data-shell-keyboard")) return false;
    if (el->getAttribute("data-shell-keyboard") == "false") return false;
    const std::string* v = styleValue(el, "visibility");
    return !v || (*v != "hidden" && *v != "collapse");
}

// A rendered element in el's subtree (el included) claims the keyboard.
bool subtreeClaimsKeyboard(dom::Element* el) {
    if (prunesSubtree(el)) return false;
    if (claimsKeyboard(el)) return true;
    for (dom::Node* child : el->childNodes()) {
        if (child->nodeType() != dom::NodeType::Element) continue;
        if (subtreeClaimsKeyboard(static_cast<dom::Element*>(child))) return true;
    }
    return false;
}

bool isEditable(dom::Element* el) {
    const std::string tag = lower(el->tagName());
    if (tag == "textarea" || tag == "select") return true;
    if (tag == "input") {
        const std::string type = lower(el->getAttribute("type"));
        return type != "button" && type != "submit" && type != "reset" && type != "checkbox" &&
               type != "radio" && type != "range" && type != "color" && type != "file" &&
               type != "image" && type != "hidden";
    }
    for (dom::Element* cur = el; cur; cur = cur->parentElement()) {
        if (!cur->hasAttribute("contenteditable")) continue;
        return cur->getAttribute("contenteditable") != "false";
    }
    return false;
}

}  // namespace

bool Engine::shellClaimsPointerAt(float x, float y) {
    if (!document_ || !document_->documentElement()) return false;
    dom::Element* html = document_->documentElement();
    dom::Element* body = document_->body();
    dom::Element* hit = hitTest(x, y);
    if (!hit || hit == html || hit == body) return false;

    for (dom::Element* cur = hit; cur && cur != body && cur != html; cur = cur->parentElement()) {
        const std::string* z = styleValue(cur, "z-index");
        if (!z || z->empty() || *z == "auto") continue;
        try {
            if (std::stoi(*z) >= kShellOverlayZ) return true;
        } catch (...) {
        }
    }
    return false;
}

bool Engine::shellClaimsKeyboard() {
    if (!document_ || !document_->documentElement()) return false;
    // A key handler may have just shown an overlay (a class toggled): read
    // the style it will be drawn with, not last frame's.
    flushLayoutForRead(document_.get());

    dom::Element* active = document_->activeElement();
    if (active && active != document_->body() && active != document_->documentElement() &&
        isEditable(active)) {
        // Only while it is rendered: focus left behind in a closed overlay
        // does not hold the keyboard.
        bool rendered = true;
        for (dom::Element* cur = active; cur && rendered; cur = cur->parentElement())
            rendered = !prunesSubtree(cur);
        if (rendered) return true;
    }
    return subtreeClaimsKeyboard(document_->documentElement());
}

}  // namespace bro::engine
