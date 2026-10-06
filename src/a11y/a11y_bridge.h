#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if BRO_WITH_A11Y
#include <broa11y/broa11y.h>
#endif

namespace bro::dom {
class Document;
class Element;
class Node;
}

namespace bro::a11y {

class AccessibilityBridge {
public:
    AccessibilityBridge();
    ~AccessibilityBridge();

    AccessibilityBridge(const AccessibilityBridge&) = delete;
    AccessibilityBridge& operator=(const AccessibilityBridge&) = delete;

    /// Initialize accessibility subsystem with a document root
    bool initialize(dom::Document* document);
    void shutdown();

    /// Synchronize the DOM accessibility tree with broa11y
    void updateTree();

    /// Rebuild the accessibility tree from the DOM
    void rebuild();

    /// Notify that an element has gained input focus
    void onFocusChanged(dom::Element* focusedElement);

    /// Notify that an element's text, attributes or label changed
    void onElementChanged(dom::Element* element);

    /// Post an accessible announcement (screen reader speech)
    void announce(std::string_view message, bool assertive = false);

    /// Process outgoing accessibility events (e.g. AT-SPI2 / UIA / NSAccessibility)
    void processEvents();

    bool isRunning() const { return initialized_; }

#if BRO_WITH_A11Y
    broa11y::Tree* tree() const { return tree_.get(); }
    broa11y::Bridge* platformBridge() const { return bridge_.get(); }
    broa11y::NodeId nodeIdForElement(const dom::Element* el) const {
        auto it = elementMap_.find(el);
        return it != elementMap_.end() ? it->second : broa11y::kInvalidNodeId;
    }
    broa11y::Node* nodeForElement(const dom::Element* el) const {
        if (!tree_) return nullptr;
        auto it = elementMap_.find(el);
        return it != elementMap_.end() ? tree_->get_node(it->second) : nullptr;
    }
#endif

private:
#if BRO_WITH_A11Y
    broa11y::Role mapElementRole(const dom::Element* el) const;
    void syncElementSubtree(dom::Element* el, broa11y::NodeId parentId);
    std::string computeAccessibleName(const dom::Element* el) const;
    void updateElementState(dom::Element* el, broa11y::Node* a11yNode);

    std::unique_ptr<broa11y::Tree> tree_;
    std::unique_ptr<broa11y::Bridge> bridge_;
    std::unordered_map<const dom::Element*, broa11y::NodeId> elementMap_;
#endif
    dom::Document* document_ = nullptr;
    bool initialized_ = false;
};

} // namespace bro::a11y
