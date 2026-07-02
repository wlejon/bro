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
            auto* parentNode = elem_->parentNode();
            if (parentNode && parentNode->nodeType() == dom::NodeType::DocumentFragment) {
                auto* sr = static_cast<dom::ShadowRoot*>(parentNode);
                if (sr->host()) return getOrCreate(sr->host());
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

    bool isHovered() const override { return elem_ && elem_ == hoveredElement_; }
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
    static inline thread_local std::unordered_map<dom::Element*, std::unique_ptr<ElementRefAdapter>> cache_;
};

} // namespace bro::layout
