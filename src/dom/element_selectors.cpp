// CSS selector matching on DOM elements (querySelector, querySelectorAll,
// matches, closest, and simple selector helpers), split from element.cpp.

#include "dom/element.h"
#include "dom/document.h"
#include "dom/text_node.h"
#include "layout/element_ref_adapter.h"
#include "css/selector.h"

#include <cctype>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::dom {

// Parsing a selector string into the compiled Selector list is not free — a
// tokenize + parse per call — and querySelector/matches/closest are hammered in
// tight loops with the same handful of literal selectors. Cache the compiled
// result keyed by the source text. The parsed Selectors are pure value data
// (no element pointers), so the cache is valid for the process; it is bounded so
// a program generating unique selectors can't grow it without limit.
static const std::vector<htmlayout::css::Selector>&
cachedParseSelectorList(const std::string& selector) {
    static thread_local std::unordered_map<std::string,
                                            std::vector<htmlayout::css::Selector>> cache;
    auto it = cache.find(selector);
    if (it != cache.end()) return it->second;
    if (cache.size() >= 1024) cache.clear();   // bound for dynamically-built selectors
    auto res = cache.emplace(selector, htmlayout::css::parseSelectorList(selector));
    return res.first->second;
}

std::vector<Element*> Element::querySelectorAll(const std::string& selector) {
    std::vector<Element*> result;
    const auto& selectors = cachedParseSelectorList(selector);
    layout::ElementRefAdapter::ScopingRootGuard scope(this, selector);  // :scope is this element

    std::function<void(Element*)> search = [&](Element* elem) {
        for (auto* child : elem->children()) {
            auto* adapter = layout::ElementRefAdapter::getOrCreate(child);
            for (auto& sel : selectors) {
                if (sel.matches(*adapter)) {
                    result.push_back(child);
                    break;
                }
            }
            search(child);
        }
    };
    search(this);
    layout::ElementRefAdapter::clearCache();
    return result;
}

Element* Element::querySelector(const std::string& selector) {
    const auto& selectors = cachedParseSelectorList(selector);
    layout::ElementRefAdapter::ScopingRootGuard scope(this, selector);  // :scope is this element

    std::function<Element*(Element*)> search = [&](Element* elem) -> Element* {
        for (auto* child : elem->children()) {
            auto* adapter = layout::ElementRefAdapter::getOrCreate(child);
            for (auto& sel : selectors) {
                if (sel.matches(*adapter)) {
                    layout::ElementRefAdapter::clearCache();
                    return child;
                }
            }
            auto* found = search(child);
            if (found) return found;
        }
        return nullptr;
    };
    auto* result = search(this);
    layout::ElementRefAdapter::clearCache();
    return result;
}

void Element::querySelectorAllUnder(Node* root, const std::string& selector,
                                    std::vector<Element*>& out, bool firstOnly) {
    if (!root) return;
    const auto& selectors = cachedParseSelectorList(selector);
    // A shadow root or fragment is not an element, so no element is :scope.
    layout::ElementRefAdapter::ScopingRootGuard scope(nullptr, selector);

    std::function<bool(Node*)> search = [&](Node* parent) -> bool {
        for (auto* node : parent->childNodes()) {
            if (node->nodeType() != NodeType::Element) continue;
            auto* child = static_cast<Element*>(node);
            auto* adapter = layout::ElementRefAdapter::getOrCreate(child);
            for (auto& sel : selectors) {
                if (sel.matches(*adapter)) {
                    out.push_back(child);
                    if (firstOnly) return true;
                    break;
                }
            }
            if (search(child)) return true;
        }
        return false;
    };
    search(root);
    layout::ElementRefAdapter::clearCache();
}

bool Element::matches(const std::string& selector) const {
    return matchesScoped(selector, this);
}

bool Element::matchesScoped(const std::string& selector, const Element* scopingRoot) const {
    const auto& selectors = cachedParseSelectorList(selector);
    layout::ElementRefAdapter::ScopingRootGuard scope(const_cast<Element*>(scopingRoot), selector);
    auto* adapter = layout::ElementRefAdapter::getOrCreate(const_cast<Element*>(this));
    bool matched = false;
    for (auto& sel : selectors) {
        if (sel.matches(*adapter)) {
            matched = true;
            break;
        }
    }
    layout::ElementRefAdapter::clearCache();
    return matched;
}

Element* Element::closest(const std::string& selector) {
    // :scope stays this element while the walk tests its ancestors.
    Element* current = this;
    while (current) {
        if (current->matchesScoped(selector, this)) return current;
        current = current->parentElement();
    }
    return nullptr;
}

// Simple CSS selector matching for dynamically created elements.
// Supports: tag, .class, #id, tag.class, .class1.class2, tag#id
bool Element::matchesSimple(const std::string& selector) const {
    if (selector.empty()) return false;
    if (selector[0] == ':') return false;

    std::string reqTag, reqId;
    std::vector<std::string> reqClasses;

    std::string current;
    enum Part { TAG, CLASS, ID } part = TAG;

    for (size_t i = 0; i <= selector.size(); ++i) {
        char c = (i < selector.size()) ? selector[i] : '\0';
        if (c == '.' || c == '#' || c == '\0') {
            if (!current.empty()) {
                if (part == TAG) reqTag = current;
                else if (part == CLASS) reqClasses.push_back(current);
                else if (part == ID) reqId = current;
            }
            current.clear();
            if (c == '.') part = CLASS;
            else if (c == '#') part = ID;
        } else {
            current += c;
        }
    }

    if (!reqTag.empty()) {
        std::string upper = reqTag;
        for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (tag_ != upper) return false;
    }

    if (!reqId.empty() && getAttribute("id") != reqId) return false;

    if (!reqClasses.empty()) {
        const std::string& cls = getAttribute("class");   // no copy
        auto isSpace = [](char c) {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
        };
        auto hasClass = [&](const std::string& want) {
            size_t i = 0, n = cls.size();
            while (i < n) {
                while (i < n && isSpace(cls[i])) ++i;
                size_t start = i;
                while (i < n && !isSpace(cls[i])) ++i;
                if (i - start == want.size() && cls.compare(start, i - start, want) == 0)
                    return true;
            }
            return false;
        };
        for (auto& rc : reqClasses) {
            if (!hasClass(rc)) return false;
        }
    }
    return true;
}

void Element::querySelectorAllSimple(const std::string& selector, std::vector<Element*>& out) {
    std::string simpleSelector = selector;
    while (!simpleSelector.empty() && simpleSelector.front() == ' ') simpleSelector.erase(0, 1);
    while (!simpleSelector.empty() && simpleSelector.back() == ' ') simpleSelector.pop_back();
    auto spacePos = simpleSelector.rfind(' ');
    if (spacePos != std::string::npos) simpleSelector = simpleSelector.substr(spacePos + 1);
    auto gtPos = simpleSelector.rfind('>');
    if (gtPos != std::string::npos) {
        simpleSelector = simpleSelector.substr(gtPos + 1);
        while (!simpleSelector.empty() && simpleSelector.front() == ' ') simpleSelector.erase(0, 1);
    }

    for (auto& child : children_) {
        if (child->nodeType() == NodeType::Element) {
            auto* elem = static_cast<Element*>(child);
            if (elem->matchesSimple(simpleSelector)) {
                out.push_back(elem);
            }
            elem->querySelectorAllSimple(selector, out);
        }
    }
}

Element* Element::querySelectorSimple(const std::string& selector) {
    std::vector<Element*> results;
    querySelectorAllSimple(selector, results);
    return results.empty() ? nullptr : results[0];
}

}  // namespace bro::dom
