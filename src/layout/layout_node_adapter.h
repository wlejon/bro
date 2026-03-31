#pragma once

#include "layout/box.h"
#include "dom/element.h"
#include "dom/text_node.h"
#include "dom/node.h"
#include "dom/shadow_root.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace bro::layout {

// Forward
class LayoutNodeAdapter;

// Adapts bro::dom nodes to htmlayout::layout::LayoutNode for layout computation.
// After layoutTree(), each adapter's box is written back to the underlying Element.
class LayoutNodeAdapter : public htmlayout::layout::LayoutNode {
public:
    // Construct for an element node
    explicit LayoutNodeAdapter(dom::Element* elem)
        : elem_(elem), textNode_(nullptr) {
        // Copy the element's stored layout box so hit testing works
        box = elem->layoutBox();
    }

    // Construct for a text node
    explicit LayoutNodeAdapter(dom::TextNode* text, dom::Element* parentElem)
        : elem_(nullptr), textNode_(text), parentElem_(parentElem) {}

    dom::Element* element() const { return elem_; }
    dom::TextNode* textNodePtr() const { return textNode_; }

    std::string tagName() const override {
        if (elem_) return elem_->tagName();
        return "#text";
    }

    bool isTextNode() const override { return textNode_ != nullptr; }

    std::string textContent() const override {
        if (textNode_) return textNode_->data();
        return "";
    }

    LayoutNode* parent() const override { return parent_; }

    std::vector<LayoutNode*> children() const override {
        std::vector<LayoutNode*> result;
        result.reserve(children_.size());
        for (auto& child : children_) {
            result.push_back(child.get());
        }
        return result;
    }

    const htmlayout::css::ComputedStyle& computedStyle() const override {
        if (elem_) return elem_->computedStyle();
        // Text nodes inherit parent's style
        if (parentElem_) return parentElem_->computedStyle();
        static const htmlayout::css::ComputedStyle empty;
        return empty;
    }

    // Write layout results back to the DOM element
    void syncBoxToElement() {
        if (elem_) {
            elem_->setLayoutBox(box);
        }
        for (auto& child : children_) {
            child->syncBoxToElement();
        }
    }

    // Build a layout tree from a DOM element tree.
    // This handles shadow DOM composed children and slot distribution.
    static std::unique_ptr<LayoutNodeAdapter> buildTree(dom::Element* root) {
        auto node = std::make_unique<LayoutNodeAdapter>(root);
        buildChildren(node.get(), root);
        return node;
    }

private:
    static void buildChildren(LayoutNodeAdapter* parent, dom::Element* elem) {
        // If element has shadow DOM, use composed children
        std::vector<dom::Node*> childNodes;
        if (elem->hasShadow()) {
            auto* sr = elem->shadowRoot();
            if (!sr->slotsValid()) sr->distributeSlots();
            childNodes = sr->composedChildren();
        } else {
            childNodes = elem->childNodes();
        }

        for (auto* childNode : childNodes) {
            if (childNode->nodeType() == dom::NodeType::Element) {
                auto* childElem = static_cast<dom::Element*>(childNode);
                // Skip display:none elements
                auto& style = childElem->computedStyle();
                auto it = style.find("display");
                if (it != style.end() && it->second == "none") continue;

                auto child = std::make_unique<LayoutNodeAdapter>(childElem);
                child->parent_ = parent;
                buildChildren(child.get(), childElem);
                parent->children_.push_back(std::move(child));
            } else if (childNode->nodeType() == dom::NodeType::Text) {
                auto* textNode = static_cast<dom::TextNode*>(childNode);
                if (textNode->data().empty()) continue;
                auto child = std::make_unique<LayoutNodeAdapter>(textNode, elem);
                child->parent_ = parent;
                parent->children_.push_back(std::move(child));
            }
        }
    }

    dom::Element* elem_ = nullptr;
    dom::TextNode* textNode_ = nullptr;
    dom::Element* parentElem_ = nullptr;  // for text nodes: their parent element
    LayoutNodeAdapter* parent_ = nullptr;
    std::vector<std::unique_ptr<LayoutNodeAdapter>> children_;
};

} // namespace bro::layout
