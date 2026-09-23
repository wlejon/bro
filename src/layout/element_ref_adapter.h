#pragma once

#include "css/selector.h"
#include "dom/element.h"
#include "dom/node.h"
#include "dom/shadow_root.h"
#include <string>
#include <string_view>
#include <span>
#include <vector>
#include <unordered_map>

namespace bro::layout {

// Adapts bro::dom::Element to htmlayout::css::ElementRef for selector matching
// and style resolution. Adapters are lightweight wrappers created on demand.
class ElementRefAdapter : public htmlayout::css::ElementRef {
public:
    explicit ElementRefAdapter(dom::Element* elem) : elem_(elem) {}

    dom::Element* element() const { return elem_; }

    std::string_view tagName() const override {
        return elem_ ? std::string_view{elem_->tagName()} : std::string_view{};
    }

    std::string_view id() const override {
        return elem_ ? std::string_view{elem_->id()} : std::string_view{};
    }

    std::string_view className() const override {
        return elem_ ? std::string_view{elem_->className()} : std::string_view{};
    }

    std::string_view getAttribute(std::string_view name) const override {
        if (!elem_) return {};
        return std::string_view{elem_->getAttribute(std::string{name})};
    }

    bool hasAttribute(std::string_view name) const override {
        if (!elem_) return false;
        // Element::hasAttribute special-cases "style" (lives in StyleProxy,
        // not attributes_) — go through it rather than checking the map directly.
        return elem_->hasAttribute(std::string{name});
    }

    ElementRef* parent() const override {
        if (!elem_) return nullptr;
        auto* p = elem_->parentElement();
        if (!p) {
            // If the parent is a ShadowRoot (not an Element), cross to the shadow host.
            // This allows :host(...) descendant selectors to walk up to the host.
            // A plain DocumentFragment (template content, createDocumentFragment)
            // shares the node type, so the cast has to be checked. A DOM query
            // (ScopingRootGuard) stays inside its own tree, as the selector
            // APIs specify; only the cascade crosses to the host.
            auto* parentNode = elem_->parentNode();
            if ((!inDomQuery_ || queryCrossesHost_) && parentNode &&
                parentNode->nodeType() == dom::NodeType::DocumentFragment) {
                auto* sr = dynamic_cast<dom::ShadowRoot*>(parentNode);
                if (sr && sr->host()) return getOrCreate(sr->host());
            }
            return nullptr;
        }
        return getOrCreate(p);
    }

    std::span<ElementRef* const> children() const override {
        childrenView_.clear();
        if (!elem_) return {};
        for (auto* child : elem_->children()) {
            childrenView_.push_back(getOrCreate(child));
        }
        return std::span<ElementRef* const>{childrenView_.data(), childrenView_.size()};
    }

    bool hasTextChildren() const override {
        if (!elem_) return false;
        for (auto* node : elem_->childNodes()) {
            if (node->nodeType() == dom::NodeType::Text) return true;
        }
        return false;
    }

    int childIndex() const override {
        if (!elem_ || !elem_->parentNode()) return 0;
        auto& siblings = elem_->parentNode()->childNodes();
        int idx = 0;
        for (auto* sib : siblings) {
            if (sib->nodeType() != dom::NodeType::Element) continue;
            if (sib == elem_) return idx;
            idx++;
        }
        return 0;
    }

    int childIndexOfType() const override {
        if (!elem_ || !elem_->parentNode()) return 0;
        auto& siblings = elem_->parentNode()->childNodes();
        int idx = 0;
        for (auto* sib : siblings) {
            if (sib->nodeType() != dom::NodeType::Element) continue;
            auto* sibEl = static_cast<dom::Element*>(sib);
            if (sibEl == elem_) return idx;
            if (sibEl->tagName() == elem_->tagName()) idx++;
        }
        return 0;
    }

    int siblingCount() const override {
        if (!elem_ || !elem_->parentNode()) return 1;
        int count = 0;
        for (auto* sib : elem_->parentNode()->childNodes()) {
            if (sib->nodeType() == dom::NodeType::Element) count++;
        }
        return count;
    }

    int siblingCountOfType() const override {
        if (!elem_ || !elem_->parentNode()) return 1;
        int count = 0;
        for (auto* sib : elem_->parentNode()->childNodes()) {
            if (sib->nodeType() != dom::NodeType::Element) continue;
            if (static_cast<dom::Element*>(sib)->tagName() == elem_->tagName()) count++;
        }
        return count;
    }

    bool isHovered() const override {
        // :hover matches the element directly under the pointer AND all of its
        // ancestors (CSS Selectors L4). Walk up from the hovered element and
        // match if elem_ is anywhere on that chain — otherwise hovering a
        // child (e.g. a row's text span) would fail to :hover the parent row.
        if (!elem_ || !hoveredElement_) return false;
        for (auto* node = static_cast<dom::Node*>(hoveredElement_); node;
             node = node->parentNode()) {
            if (node == elem_) return true;
        }
        return false;
    }
    bool isFocused() const override {
        return elem_ && elem_->document() && elem_ == elem_->document()->activeElement();
    }
    bool isActive() const override { return elem_ && elem_ == activeElement_; }

    bool isLink() const override {
        if (!elem_) return false;
        std::string tag = elem_->tagName();
        return (tag == "A" || tag == "a" || tag == "AREA" || tag == "area") &&
               elem_->hasAttribute("href");
    }
    bool isVisited() const override { return false; } // no visit tracking

    bool isFocusWithin() const override {
        if (!elem_ || !elem_->document()) return false;
        auto* active = elem_->document()->activeElement();
        if (!active) return false;
        // Walk up from active element to see if it's a descendant of this element
        auto* node = static_cast<dom::Node*>(active);
        while (node) {
            if (node == elem_) return true;
            node = node->parentNode();
        }
        return false;
    }
    bool isFocusVisible() const override { return isFocused(); }

    // Container queries — type/name come from the element's computed style,
    // sizes from its last layout box. Style resolution runs before layout, so
    // the consumer must re-resolve styles once after layout when container
    // queries are in use (the sizes here are one layout pass behind).
    std::string_view containerType() const override {
        if (!elem_) return "none";
        const auto& cs = elem_->computedStyle();
        auto it = cs.find("container-type");
        if (it == cs.end() || it->second.empty()) return "none";
        return it->second;
    }
    std::string_view containerName() const override {
        if (!elem_) return "";
        const auto& cs = elem_->computedStyle();
        auto it = cs.find("container-name");
        if (it == cs.end() || it->second == "none") return "";
        return it->second;
    }
    float containerInlineSize() const override {
        return elem_ ? elem_->layoutBox().contentRect.width : 0.0f;
    }
    float containerBlockSize() const override {
        return elem_ ? elem_->layoutBox().contentRect.height : 0.0f;
    }

    bool isChecked() const override {
        if (!elem_) return false;
        return elem_->hasAttribute("checked");
    }
    bool isDisabled() const override {
        if (!elem_) return false;
        return elem_->hasAttribute("disabled");
    }
    bool isEnabled() const override { return !isDisabled(); }

    bool isRequired() const override {
        if (!elem_) return false;
        return elem_->hasAttribute("required");
    }
    bool isOptional() const override { return !isRequired(); }

    bool isReadOnly() const override {
        if (!elem_) return false;
        return elem_->hasAttribute("readonly");
    }
    bool isReadWrite() const override {
        if (!elem_) return false;
        std::string tag = elem_->tagName();
        if (tag == "INPUT" || tag == "input" || tag == "TEXTAREA" || tag == "textarea")
            return !isReadOnly() && !isDisabled();
        return elem_->hasAttribute("contenteditable");
    }

    bool isPlaceholderShown() const override {
        if (!elem_) return false;
        if (!elem_->hasAttribute("placeholder")) return false;
        std::string val = elem_->getAttribute("value");
        return val.empty();
    }

    bool isIndeterminate() const override {
        if (!elem_) return false;
        return elem_->hasAttribute("indeterminate");
    }

    bool isTarget() const override { return false; } // no URL fragment tracking

    // :modal — a top-layer entry that blocks the rest of the document (a
    // dialog opened with showModal()).
    bool isModal() const override {
        if (!elem_ || !elem_->document()) return false;
        for (const auto& e : elem_->document()->topLayer()) {
            if (e.element == elem_) return e.modal;
        }
        return false;
    }

    void* scope() const override {
        if (!elem_) return nullptr;
        return static_cast<void*>(elem_->containingShadowRoot());
    }

    void* shadowRoot() const override {
        if (!elem_) return nullptr;
        return static_cast<void*>(elem_->shadowRoot());
    }

    ElementRef* assignedSlot() const override {
        if (!elem_) return nullptr;
        // An element is slotted if its parent is a shadow host
        auto* parent = elem_->parentElement();
        if (!parent || !parent->hasShadow()) return nullptr;
        auto* sr = parent->shadowRoot();
        if (!sr) return nullptr;
        auto* slot = sr->assignedSlot(elem_);
        if (!slot) return nullptr;
        return getOrCreate(slot);
    }

    std::string_view partName() const override {
        if (!elem_) return {};
        return std::string_view{elem_->getAttribute("part")};
    }

    // :scope. The cascade leaves it the root element (htmlayout's default);
    // querySelector / querySelectorAll / matches / closest name the element
    // they were called on for the duration of the match (ScopingRootGuard).
    bool isScopingRoot() const override {
        if (hasScopingRoot_) return scopingRoot_ != nullptr && elem_ == scopingRoot_;
        return ElementRef::isScopingRoot();
    }

    // A DOM query (querySelector/All, matches, closest) for its duration:
    // names `el` as :scope (null: no element is, as for a shadow root or a
    // fragment), and keeps combinators inside the element's own tree rather
    // than walking from a shadow tree out to its host — `div span` inside a
    // shadow tree must not match through a <div> host. A selector naming
    // :host / :host-context still reaches the host, which is what those
    // pseudo-classes test (htmlayout matches :host on any shadow host).
    // Restores the previous state when destroyed (a matches() nested in
    // another query, say).
    class ScopingRootGuard {
    public:
        ScopingRootGuard(dom::Element* el, std::string_view selectorText)
            : prev_(scopingRoot_), prevInQuery_(inDomQuery_),
              prevHasRoot_(hasScopingRoot_), prevCrosses_(queryCrossesHost_) {
            scopingRoot_ = el;
            hasScopingRoot_ = true;
            inDomQuery_ = true;
            queryCrossesHost_ = selectorText.find(":host") != std::string_view::npos;
        }
        ~ScopingRootGuard() {
            scopingRoot_ = prev_;
            hasScopingRoot_ = prevHasRoot_;
            inDomQuery_ = prevInQuery_;
            queryCrossesHost_ = prevCrosses_;
        }
        ScopingRootGuard(const ScopingRootGuard&) = delete;
        ScopingRootGuard& operator=(const ScopingRootGuard&) = delete;
    private:
        dom::Element* prev_;
        bool prevInQuery_;
        bool prevHasRoot_;
        bool prevCrosses_;
    };

    // Global state setters (called by Engine before style resolution)
    static void setHoveredElement(dom::Element* el) { hoveredElement_ = el; }
    static void setActiveElement(dom::Element* el) { activeElement_ = el; }

    // Clear the adapter cache (call between frames or after DOM mutations)
    static void clearCache() { cache_.clear(); }

    // Get or create an adapter for an element
    static ElementRefAdapter* getOrCreate(dom::Element* elem) {
        auto it = cache_.find(elem);
        if (it != cache_.end()) return it->second.get();
        auto adapter = std::make_unique<ElementRefAdapter>(elem);
        auto* raw = adapter.get();
        cache_[elem] = std::move(adapter);
        return raw;
    }

private:
    dom::Element* elem_;
    mutable std::vector<ElementRef*> childrenView_;

    static inline thread_local dom::Element* hoveredElement_ = nullptr;
    static inline thread_local dom::Element* activeElement_ = nullptr;
    static inline thread_local dom::Element* scopingRoot_ = nullptr;
    static inline thread_local bool hasScopingRoot_ = false;
    static inline thread_local bool inDomQuery_ = false;
    static inline thread_local bool queryCrossesHost_ = false;
    static inline thread_local std::unordered_map<dom::Element*, std::unique_ptr<ElementRefAdapter>> cache_;
};

} // namespace bro::layout
