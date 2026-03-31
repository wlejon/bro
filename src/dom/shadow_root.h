#pragma once
#include "dom/node.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace bro::dom {

class Element;
class Document;

class ShadowRoot : public Node {
public:
    enum class Mode { Open, Closed };

    explicit ShadowRoot(Element* host, Mode mode);
    ~ShadowRoot() override = default;

    NodeType nodeType() const override { return NodeType::DocumentFragment; }
    std::string nodeName() const override { return "#shadow-root"; }

    Element* host() const { return host_; }
    Mode mode() const { return mode_; }
    std::string modeString() const { return mode_ == Mode::Open ? "open" : "closed"; }

    // Unique scope ID for CSS encapsulation
    std::string scopeId() const { return "s" + std::to_string(nodeId()); }

    // Shadow-scoped queries (only search within shadow tree)
    Element* getElementById(const std::string& id);
    Element* querySelector(const std::string& selector);
    std::vector<Element*> querySelectorAll(const std::string& selector);

    // innerHTML for shadow root
    std::string innerHTML() const;
    void setInnerHTML(const std::string& html, Document* doc);

    // Style encapsulation
    void addStyleSheet(const std::string& cssText);
    const std::vector<std::string>& styleSheets() const { return styleSheets_; }

    // Generate scoped CSS (prefixes all selectors with scope attribute)
    std::string scopedCSS() const;

    // Parsed CSS rule for inline style application
    struct CSSRule {
        std::string selector;  // original selector (e.g. ".card-title", ":host")
        std::string properties; // CSS declarations (e.g. "color: red; font-size: 14px")
    };

    // Parse styleSheets into rules
    std::vector<CSSRule> parsedRules() const;

    // Resolve inline styles for a given element by matching shadow CSS rules
    std::string resolveInlineStyles(Element* elem) const;

    // Get :host styles
    std::string hostStyles() const;

    // Slot distribution
    struct SlotAssignment {
        Element* slot = nullptr;
        std::vector<Node*> assignedNodes;
    };

    // Recompute slot assignments from light DOM children
    void distributeSlots();

    // Get nodes assigned to a specific slot element
    std::vector<Node*> assignedNodes(Element* slot) const;

    // Get the slot element a light DOM node is assigned to
    Element* assignedSlot(Node* node) const;

    // Get all slot elements in shadow tree
    std::vector<Element*> findSlots() const;

    // Build the composed (flattened) child list for rendering:
    // shadow tree structure with <slot> elements replaced by their assigned content
    std::vector<Node*> composedChildren() const;

    // Mark that slot distribution needs recalculating
    void invalidateSlots() { slotsValid_ = false; }
    bool slotsValid() const { return slotsValid_; }

private:
    void collectSlots(Node* node, std::vector<Element*>& slots) const;
    void collectElements(Node* node, const std::string& selector,
                         std::vector<Element*>& out) const;

    Element* host_;
    Mode mode_;
    std::vector<std::string> styleSheets_;

    // Slot distribution cache
    mutable bool slotsValid_ = false;
    mutable std::vector<SlotAssignment> slotAssignments_;
    mutable std::unordered_map<Node*, Element*> nodeToSlot_;
};

} // namespace bro::dom
