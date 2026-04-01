#include "dom/shadow_root.h"
#include "dom/element.h"
#include "dom/document.h"
#include "dom/text_node.h"
#include "dom/comment_node.h"

#include <sstream>
#include <algorithm>
#include <cctype>

namespace bro::dom {

ShadowRoot::ShadowRoot(Element* host, Mode mode)
    : host_(host), mode_(mode) {}

// ---------------------------------------------------------------------------
// Shadow-scoped queries
// ---------------------------------------------------------------------------

Element* ShadowRoot::getElementById(const std::string& id) {
    for (auto* child : children_) {
        if (child->nodeType() == NodeType::Element) {
            auto* elem = static_cast<Element*>(child);
            if (elem->id() == id) return elem;
            // Recurse
            std::vector<Element*> results;
            collectElements(child, "#" + id, results);
            if (!results.empty()) return results[0];
        }
    }
    return nullptr;
}

Element* ShadowRoot::querySelector(const std::string& selector) {
    auto results = querySelectorAll(selector);
    return results.empty() ? nullptr : results[0];
}

std::vector<Element*> ShadowRoot::querySelectorAll(const std::string& selector) {
    std::vector<Element*> results;
    for (auto* child : children_) {
        collectElements(child, selector, results);
    }
    return results;
}

void ShadowRoot::collectElements(Node* node, const std::string& selector,
                                  std::vector<Element*>& out) const {
    if (!node || node->nodeType() != NodeType::Element) return;
    auto* elem = static_cast<Element*>(node);
    if (elem->matchesSimple(selector)) {
        out.push_back(elem);
    }
    // Don't cross into nested shadow roots
    if (elem->hasShadow()) return;
    for (auto* child : elem->childNodes()) {
        collectElements(child, selector, out);
    }
}

// ---------------------------------------------------------------------------
// innerHTML
// ---------------------------------------------------------------------------

std::string ShadowRoot::innerHTML() const {
    std::ostringstream oss;
    for (const auto* child : children_) {
        if (child->nodeType() == NodeType::Text) {
            auto* text = static_cast<const TextNode*>(child);
            oss << text->data();
        } else if (child->nodeType() == NodeType::Comment) {
            auto* comment = static_cast<const CommentNode*>(child);
            oss << "<!--" << comment->data() << "-->";
        } else if (child->nodeType() == NodeType::Element) {
            auto* elem = static_cast<const Element*>(child);
            oss << elem->outerHTML();
        }
    }
    return oss.str();
}

void ShadowRoot::setInnerHTML(const std::string& html, Document* doc) {
    // Clear existing children
    for (auto* child : children_) {
        child->setParent(nullptr);
        if (doc) doc->freeNode(child);
    }
    children_.clear();
    styleSheets_.clear();
    invalidateSlots();

    if (html.empty() || !doc) return;

    // Parse HTML via the document's parseInnerHTML mechanism.
    // We create a temporary element, parse into it, then steal its children.
    auto* temp = doc->createElement("div");
    doc->parseInnerHTML(temp, html);

    // Move children from temp to shadow root
    auto kids = temp->childNodes();
    for (auto* child : kids) {
        child->setParent(nullptr);
    }
    temp->childNodes().clear();

    for (auto* child : kids) {
        appendChild(child);
        // Extract <style> elements for scoped CSS
        if (child->nodeType() == NodeType::Element) {
            auto* elem = static_cast<Element*>(child);
            if (elem->tagName() == "STYLE") {
                styleSheets_.push_back(elem->textContent());
            }
        }
    }

    // Free the temp element
    doc->freeNode(temp);
}

// ---------------------------------------------------------------------------
// Style encapsulation — scope CSS selectors
// ---------------------------------------------------------------------------

void ShadowRoot::addStyleSheet(const std::string& cssText) {
    styleSheets_.push_back(cssText);
}

std::string ShadowRoot::scopedCSS() const {
    if (styleSheets_.empty()) return {};

    std::string scope = "[data-bro-shadow=\"" + scopeId() + "\"]";
    std::string result;

    for (auto& css : styleSheets_) {
        // Simple CSS scoping: prefix each rule's selector with the scope attribute.
        // Parse by finding { and } to identify rule boundaries.
        size_t pos = 0;
        while (pos < css.size()) {
            // Skip whitespace
            while (pos < css.size() && std::isspace(static_cast<unsigned char>(css[pos])))
                pos++;
            if (pos >= css.size()) break;

            // Find the opening brace
            size_t braceStart = css.find('{', pos);
            if (braceStart == std::string::npos) break;

            std::string selector = css.substr(pos, braceStart - pos);

            // Find matching closing brace
            size_t braceEnd = css.find('}', braceStart);
            if (braceEnd == std::string::npos) break;

            std::string body = css.substr(braceStart, braceEnd - braceStart + 1);

            // Trim selector
            while (!selector.empty() && std::isspace(static_cast<unsigned char>(selector.front())))
                selector.erase(0, 1);
            while (!selector.empty() && std::isspace(static_cast<unsigned char>(selector.back())))
                selector.pop_back();

            if (!selector.empty()) {
                // Handle comma-separated selectors
                std::istringstream ss(selector);
                std::string part;
                bool first = true;
                while (std::getline(ss, part, ',')) {
                    // Trim
                    while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front())))
                        part.erase(0, 1);
                    while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back())))
                        part.pop_back();

                    if (!first) result += ", ";
                    first = false;

                    // :host selector maps to the host element itself
                    if (part == ":host") {
                        result += scope;
                    } else if (part.substr(0, 6) == ":host(") {
                        // :host(.foo) -> [data-bro-shadow="sN"].foo
                        std::string inner = part.substr(6, part.size() - 7);
                        result += scope + inner;
                    } else {
                        // Normal selector: scope all parts
                        result += scope + " " + part;
                    }
                }
                result += " " + body + "\n";
            }

            pos = braceEnd + 1;
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// CSS rule parsing and inline style resolution
// ---------------------------------------------------------------------------

std::vector<ShadowRoot::CSSRule> ShadowRoot::parsedRules() const {
    std::vector<CSSRule> rules;
    for (auto& css : styleSheets_) {
        size_t pos = 0;
        while (pos < css.size()) {
            // Skip whitespace
            while (pos < css.size() && std::isspace(static_cast<unsigned char>(css[pos])))
                pos++;
            if (pos >= css.size()) break;

            size_t braceStart = css.find('{', pos);
            if (braceStart == std::string::npos) break;

            size_t braceEnd = css.find('}', braceStart);
            if (braceEnd == std::string::npos) break;

            std::string selector = css.substr(pos, braceStart - pos);
            std::string body = css.substr(braceStart + 1, braceEnd - braceStart - 1);

            // Trim
            while (!selector.empty() && std::isspace(static_cast<unsigned char>(selector.front())))
                selector.erase(0, 1);
            while (!selector.empty() && std::isspace(static_cast<unsigned char>(selector.back())))
                selector.pop_back();
            while (!body.empty() && std::isspace(static_cast<unsigned char>(body.front())))
                body.erase(0, 1);
            while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back())))
                body.pop_back();

            if (!selector.empty() && !body.empty()) {
                // Handle comma-separated selectors
                std::istringstream ss(selector);
                std::string part;
                while (std::getline(ss, part, ',')) {
                    while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front())))
                        part.erase(0, 1);
                    while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back())))
                        part.pop_back();
                    if (!part.empty()) {
                        rules.push_back({part, body});
                    }
                }
            }
            pos = braceEnd + 1;
        }
    }
    return rules;
}

// Simple selector match: checks if an element matches a CSS selector.
// Supports: .class, #id, tag, tag.class, .class1.class2
static bool matchesCSSSelector(const Element* elem, const std::string& selector) {
    if (selector.empty()) return false;
    // We only match the last part of descendant selectors for now
    std::string sel = selector;
    auto spacePos = sel.rfind(' ');
    if (spacePos != std::string::npos) sel = sel.substr(spacePos + 1);

    return elem->matchesSimple(sel);
}

std::string ShadowRoot::resolveInlineStyles(Element* elem) const {
    auto rules = parsedRules();
    std::string result;
    for (auto& rule : rules) {
        if (rule.selector == ":host" || rule.selector.substr(0, 6) == ":host(")
            continue; // :host handled separately
        if (matchesCSSSelector(elem, rule.selector)) {
            if (!result.empty() && result.back() != ';')
                result += "; ";
            result += rule.properties;
        }
    }
    return result;
}

std::string ShadowRoot::hostStyles() const {
    auto rules = parsedRules();
    std::string result;
    for (auto& rule : rules) {
        if (rule.selector == ":host") {
            if (!result.empty() && result.back() != ';')
                result += "; ";
            result += rule.properties;
        }
        // :host(.foo) — would need to check host's classes, skip for now
    }
    return result;
}

// ---------------------------------------------------------------------------
// Slot distribution
// ---------------------------------------------------------------------------

void ShadowRoot::collectSlots(Node* node, std::vector<Element*>& slots) const {
    if (!node) return;
    if (node->nodeType() == NodeType::Element) {
        auto* elem = static_cast<Element*>(node);
        if (elem->tagName() == "SLOT") {
            slots.push_back(elem);
        }
        // Don't descend into nested shadow roots
        if (!elem->hasShadow()) {
            for (auto* child : elem->childNodes()) {
                collectSlots(child, slots);
            }
        }
    }
}

void ShadowRoot::distributeSlots() {
    if (slotsValid_) return;

    slotAssignments_.clear();
    nodeToSlot_.clear();

    // Find all <slot> elements in shadow tree
    std::vector<Element*> slots;
    for (auto* child : children_) {
        collectSlots(child, slots);
    }

    if (slots.empty()) {
        slotsValid_ = true;
        return;
    }

    // Create assignment entries
    for (auto* slot : slots) {
        SlotAssignment sa;
        sa.slot = slot;
        slotAssignments_.push_back(sa);
    }

    // Distribute host's light DOM children to slots
    if (!host_) {
        slotsValid_ = true;
        return;
    }

    Element* defaultSlot = nullptr;
    std::unordered_map<std::string, size_t> namedSlots;

    for (size_t i = 0; i < slotAssignments_.size(); i++) {
        auto* slot = slotAssignments_[i].slot;
        std::string slotName = slot->getAttribute("name");
        if (slotName.empty()) {
            if (!defaultSlot) defaultSlot = slot;
        } else {
            namedSlots[slotName] = i;
        }
    }

    for (auto* child : host_->childNodes()) {
        std::string assignedName;
        if (child->nodeType() == NodeType::Element) {
            assignedName = static_cast<Element*>(child)->getAttribute("slot");
        }

        if (!assignedName.empty()) {
            // Named slot
            auto it = namedSlots.find(assignedName);
            if (it != namedSlots.end()) {
                slotAssignments_[it->second].assignedNodes.push_back(child);
                nodeToSlot_[child] = slotAssignments_[it->second].slot;
            }
        } else if (defaultSlot) {
            // Default slot
            for (auto& sa : slotAssignments_) {
                if (sa.slot == defaultSlot) {
                    sa.assignedNodes.push_back(child);
                    nodeToSlot_[child] = defaultSlot;
                    break;
                }
            }
        }
    }

    slotsValid_ = true;
}

std::vector<Node*> ShadowRoot::assignedNodes(Element* slot) const {
    const_cast<ShadowRoot*>(this)->distributeSlots();
    for (auto& sa : slotAssignments_) {
        if (sa.slot == slot) return sa.assignedNodes;
    }
    return {};
}

Element* ShadowRoot::assignedSlot(Node* node) const {
    const_cast<ShadowRoot*>(this)->distributeSlots();
    auto it = nodeToSlot_.find(node);
    return (it != nodeToSlot_.end()) ? it->second : nullptr;
}

std::vector<Element*> ShadowRoot::findSlots() const {
    std::vector<Element*> slots;
    for (auto* child : children_) {
        collectSlots(child, slots);
    }
    return slots;
}

std::vector<Node*> ShadowRoot::composedChildren() const {
    // The composed tree is the shadow tree, but with <slot> elements
    // replaced by their assigned nodes (or fallback content if no assignments)
    const_cast<ShadowRoot*>(this)->distributeSlots();
    return composeNodes(children_);
}

std::vector<Node*> ShadowRoot::composeNodes(const std::vector<Node*>& nodes) const {
    std::vector<Node*> result;
    for (auto* node : nodes) {
        if (node->nodeType() == NodeType::Element) {
            auto* elem = static_cast<Element*>(node);
            if (elem->tagName() == "SLOT") {
                // Replace <slot> with assigned nodes, or fallback to slot's children
                auto assigned = assignedNodes(elem);
                if (!assigned.empty()) {
                    for (auto* n : assigned)
                        result.push_back(n);
                } else {
                    // Fallback: use slot's own children
                    for (auto* child : elem->childNodes())
                        result.push_back(child);
                }
                continue;
            }
        }
        result.push_back(node);
    }
    return result;
}

} // namespace bro::dom
