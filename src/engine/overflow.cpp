#include "engine/overflow.h"
#include "dom/element.h"
#include "dom/shadow_root.h"

#include <algorithm>
#include <unordered_map>

namespace bro::engine {

std::string getOverflowY(const htmlayout::css::ComputedStyle& style) {
    auto oyIt = style.find("overflow-y");
    if (oyIt != style.end()) return oyIt->second;
    auto oIt = style.find("overflow");
    if (oIt != style.end()) return oIt->second;
    return "visible";
}

bool overflowClips(const std::string& ov) {
    return ov == "hidden" || ov == "scroll" || ov == "auto";
}

bool overflowScrollable(const std::string& ov) {
    return ov == "scroll" || ov == "auto";
}

float maxScrollTop(dom::Element* el) {
    auto& box = el->layoutBox();
    return std::max(0.0f, box.naturalHeight - box.contentRect.height);
}

dom::Element* composedParent(dom::Element* el) {
    if (!el) return nullptr;
    auto* p = el->parentNode();
    if (!p) return nullptr;
    if (p->nodeType() == dom::NodeType::Element)
        return static_cast<dom::Element*>(p);
    // Parent is a ShadowRoot — cross to the host element
    if (p->nodeType() == dom::NodeType::DocumentFragment) {
        auto* sr = dynamic_cast<dom::ShadowRoot*>(p);
        if (sr) return sr->host();
    }
    return nullptr;
}

} // namespace bro::engine
