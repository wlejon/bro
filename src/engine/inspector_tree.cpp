// Engine inspector DOM tree serialization — split from system_panels.cpp.

#include "engine/engine.h"
#include "dom/document.h"
#include "dom/element.h"
#include <cstdio>
#include <functional>
#include <string>

namespace bro::engine {

namespace {

bool elementInTree(dom::Element* el, dom::Element* root) {
    while (el) {
        if (el == root) return true;
        el = el->parentElement();
    }
    return false;
}

void appendJsonString(const std::string& s, std::string& out) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

// One inspector node, minus its children: the shape system/inspector.html
// walks. `id` is the per-fetch number inspectorSelectById resolves.
void appendInspectorNode(int id, dom::Element* el, bool hasChildren, std::string& out) {
    out += "{\"id\":" + std::to_string(id);
    out += ",\"tag\":"; appendJsonString(el->tagName(), out);
    out += ",\"idAttr\":"; appendJsonString(el->getAttribute("id"), out);
    out += ",\"classes\":"; appendJsonString(el->getAttribute("class"), out);
    out += ",\"hasChildren\":"; out += hasChildren ? "true" : "false";
}

} // namespace

std::string Engine::inspectorAppTreeJson(int maxDepth) {
    inspectorNodeMap_.clear();
    inspectorNextId_ = 0;
    auto* root = document_ ? document_->documentElement() : nullptr;
    if (!root) return "null";

    // Each visited element gets a fresh integer id mapped to its pointer so
    // a later inspectorSelectById can resolve back to it.
    std::string out;
    std::function<void(dom::Element*, int)> walk;
    walk = [&](dom::Element* el, int depth) {
        const int id = inspectorNextId_++;
        inspectorNodeMap_[id] = el;
        auto kids = el->children();
        appendInspectorNode(id, el, !kids.empty(), out);
        const bool emitChildren = (maxDepth < 0 || depth < maxDepth);
        if (emitChildren && !kids.empty()) {
            out += ",\"children\":[";
            bool first = true;
            for (auto* k : kids) {
                if (!k) continue;
                if (!first) out += ',';
                first = false;
                walk(k, depth + 1);
            }
            out += ']';
        }
        out += '}';
    };
    walk(root, 0);
    return out;
}

std::string Engine::inspectorChildrenJson(int parentId) {
    auto it = inspectorNodeMap_.find(parentId);
    if (it == inspectorNodeMap_.end() || !it->second) return "[]";
    auto* root = document_ ? document_->documentElement() : nullptr;
    if (!root || !elementInTree(it->second, root)) return "[]";
    std::string out = "[";
    bool first = true;
    for (auto* k : it->second->children()) {
        if (!k) continue;
        const int id = inspectorNextId_++;
        inspectorNodeMap_[id] = k;
        if (!first) out += ',';
        first = false;
        appendInspectorNode(id, k, !k->children().empty(), out);
        out += '}';
    }
    out += ']';
    return out;
}

std::string Engine::inspectorSelectedJson() {
    auto* el = inspector_.selected;
    auto* root = document_ ? document_->documentElement() : nullptr;
    if (!el || !root || !elementInTree(el, root)) {
        inspector_.selected = nullptr;
        return "null";
    }
    // The id the last fetch gave this element, if any, so the panel can keep
    // its tree row highlighted across re-fetches.
    int id = -1;
    for (auto& [k, v] : inspectorNodeMap_) {
        if (v == el) { id = k; break; }
    }
    std::string out;
    appendInspectorNode(id, el, !el->children().empty(), out);
    out += '}';
    return out;
}

void Engine::inspectorSelectById(int id) {
    auto it = inspectorNodeMap_.find(id);
    if (it == inspectorNodeMap_.end()) return;
    auto* root = document_ ? document_->documentElement() : nullptr;
    if (!root || !elementInTree(it->second, root)) return;
    inspector_.selected = it->second;
    systemDirty_ = true;
}

} // namespace bro::engine
