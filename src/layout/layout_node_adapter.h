#pragma once

#include "layout/box.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/el_select.h"
#include "layout/el_svg.h"
#include "layout/el_video.h"
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
    // Construct for an element node. The box is written entirely by layout;
    // no need to seed it from the element (the tree is persistent across
    // layouts, and hit testing reads box after layout has populated it).
    explicit LayoutNodeAdapter(dom::Element* elem)
        : elem_(elem), textNode_(nullptr) {}

    // Construct for a text node
    explicit LayoutNodeAdapter(dom::TextNode* text, dom::Element* parentElem)
        : elem_(nullptr), textNode_(text), parentElem_(parentElem) {}

    // Construct a synthetic pseudo-element wrapper (::before / ::after).
    // hostElem is the real element the pseudo belongs to; which is
    // "before" or "after". The wrapper's only child is a pseudo-text node.
    static std::unique_ptr<LayoutNodeAdapter> makePseudo(dom::Element* hostElem,
                                                          const std::string& which) {
        auto wrap = std::unique_ptr<LayoutNodeAdapter>(
            new LayoutNodeAdapter(hostElem, which, /*isText=*/false));
        auto txt = std::unique_ptr<LayoutNodeAdapter>(
            new LayoutNodeAdapter(hostElem, which, /*isText=*/true));
        txt->parent_ = wrap.get();
        wrap->children_.push_back(std::move(txt));
        return wrap;
    }

    dom::Element* element() const { return elem_; }
    dom::TextNode* textNodePtr() const { return textNode_; }

    std::string tagName() const override {
        if (pseudoHost_) return pseudoIsText_ ? "#text" : ("::" + pseudoWhich_);
        if (elem_) return elem_->tagName();
        return "#text";
    }

    bool isTextNode() const override {
        if (pseudoHost_) return pseudoIsText_;
        return textNode_ != nullptr;
    }

    std::string textContent() const override {
        if (pseudoHost_ && pseudoIsText_) return pseudoHost_->pseudoContent(pseudoWhich_);
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
        if (pseudoHost_) return pseudoHost_->pseudoStyle(pseudoWhich_);
        if (elem_) return elem_->computedStyle();
        // Text nodes inherit parent's style
        if (parentElem_) return parentElem_->computedStyle();
        static const htmlayout::css::ComputedStyle empty;
        return empty;
    }

    std::string attribute(const std::string& name) const override {
        return elem_ ? elem_->getAttribute(name) : std::string{};
    }

    LayoutNode* pseudoBefore() const override { return ensurePseudo("before"); }
    LayoutNode* pseudoAfter()  const override { return ensurePseudo("after");  }

    // bro::dom::Element only tracks vertical scroll today (scrollTop_).
    // scrollLeftPx() returns 0 until horizontal scrolling is supported.
    float scrollLeftPx() const override { return 0.0f; }
    float scrollTopPx() const override {
        return elem_ ? elem_->scrollTopValue() : 0.0f;
    }

    bool intrinsicSize(float& w, float& h, float maxWidth) const override {
        if (!elem_) return false;
        if (auto* ctrl = elem_->inputControl()) {
            ctrl->getContentSize(w, h, maxWidth);
            return true;
        }
        if (auto* ctrl = elem_->textareaControl()) {
            ctrl->getContentSize(w, h);
            return true;
        }
        if (auto* ctrl = elem_->selectControl()) {
            ctrl->getContentSize(w, h);
            return true;
        }
        if (auto* ctrl = elem_->svgControl()) {
            ctrl->getContentSize(w, h);
            return true;
        }
        if (auto* ctrl = elem_->videoControl()) {
            ctrl->getContentSize(w, h);
            return true;
        }
        // <canvas> is a replaced element with default 300x150 intrinsic size
        if (elem_->tagName() == "canvas" || elem_->tagName() == "CANVAS") {
            std::string wa = elem_->getAttribute("width");
            std::string ha = elem_->getAttribute("height");
            w = wa.empty() ? 300.0f : std::strtof(wa.c_str(), nullptr);
            h = ha.empty() ? 150.0f : std::strtof(ha.c_str(), nullptr);
            return true;
        }
        return false;
    }

    // Write layout results back to the DOM element/text node
    void syncBoxToElement() {
        if (pseudoHost_ && !pseudoIsText_) {
            // Pseudo-element wrapper: stash its computed box on the host
            // so the draw traversal can paint background/borders/text.
            // The wrapper's contentRect is the pseudo's content origin;
            // its lone text child carries the placed text runs which are
            // copied in below after recursing.
            pseudoHost_->pseudoBoxMut(pseudoWhich_) = box;
        } else if (pseudoHost_ && pseudoIsText_) {
            // Pseudo-text: lift placed text runs onto the host's pseudoBox
            // so the draw traversal can render them without walking into
            // the synthetic layout subtree.
            auto& hostBox = pseudoHost_->pseudoBoxMut(pseudoWhich_);
            hostBox.textRuns = box.textRuns;
        } else if (elem_) {
            elem_->setLayoutBox(box);
        } else if (textNode_) {
            textNode_->setLayoutBox(box);
        }
        for (auto& child : children_) {
            child->syncBoxToElement();
        }
        if (pseudoBeforeAdapter_) pseudoBeforeAdapter_->syncBoxToElement();
        if (pseudoAfterAdapter_)  pseudoAfterAdapter_->syncBoxToElement();
    }

    // Map a layout-tree node back to its backing DOM element. All nodes in
    // the tree are LayoutNodeAdapter instances (bro is the sole LayoutNode
    // instantiator), so a static_cast is safe.
    //
    // For text-node adapters, returns the parent element — text nodes aren't
    // event targets in the DOM model, so a hit on a text run resolves to the
    // containing element (matches browser elementFromPoint semantics).
    static dom::Element* elementFor(htmlayout::layout::LayoutNode* node) {
        if (!node) return nullptr;
        auto* a = static_cast<LayoutNodeAdapter*>(node);
        if (a->pseudoHost_) return a->pseudoHost_;
        if (a->elem_) return a->elem_;
        return a->parentElem_;
    }

    // Build a layout tree from a DOM element tree.
    // This handles shadow DOM composed children and slot distribution.
    static std::unique_ptr<LayoutNodeAdapter> buildTree(dom::Element* root) {
        auto node = std::make_unique<LayoutNodeAdapter>(root);
        buildChildren(node.get(), root);
        return node;
    }

private:
    // Private constructor for synthetic pseudo-element adapters.
    LayoutNodeAdapter(dom::Element* host, const std::string& which, bool isText)
        : pseudoHost_(host), pseudoWhich_(which), pseudoIsText_(isText) {}

    // Lazily build / tear down the synthetic pseudo wrapper on each layout
    // pass. Pseudo content can appear or disappear when classes change (e.g.
    // .selected moving between menu items toggles ::before/::after rules);
    // checking elem_->pseudoContent at access time keeps the layout tree in
    // sync without requiring a structural rebuild.
    LayoutNode* ensurePseudo(const std::string& which) const {
        if (!elem_) return nullptr;
        bool want = !elem_->pseudoContent(which).empty();
        auto& slot = (which == "before") ? pseudoBeforeAdapter_ : pseudoAfterAdapter_;
        if (want && !slot) {
            slot = makePseudo(elem_, which);
            slot->parent_ = const_cast<LayoutNodeAdapter*>(this);
        } else if (!want && slot) {
            slot.reset();
        }
        return slot.get();
    }

    // Get the containing shadow root for an element (walk up parents)
    static dom::ShadowRoot* containingShadow(dom::Element* elem) {
        return elem ? elem->containingShadowRoot() : nullptr;
    }

    static void buildChildren(LayoutNodeAdapter* parent, dom::Element* elem) {
        // SVG is a replaced element — its content is rendered by SkSVGDOM,
        // not by CSS layout. Don't descend into SVG children.
        if (elem->svgControl()) return;

        // If element has shadow DOM, use composed children (top-level slot replacement)
        std::vector<dom::Node*> childNodes;
        dom::ShadowRoot* sr = nullptr;
        if (elem->hasShadow()) {
            sr = elem->shadowRoot();
            if (!sr->slotsValid()) sr->distributeSlots();
            childNodes = sr->composedChildren();
        } else {
            childNodes = elem->childNodes();
        }

        // Check if we're inside a shadow tree — needed for nested slot replacement
        dom::ShadowRoot* enclosingSR = containingShadow(elem);

        for (auto* childNode : childNodes) {
            if (childNode->nodeType() == dom::NodeType::Element) {
                auto* childElem = static_cast<dom::Element*>(childNode);

                // Replace <slot> elements with their assigned nodes or fallback
                if (childElem->tagName() == "SLOT" && enclosingSR) {
                    auto assigned = enclosingSR->assignedNodes(childElem);
                    if (!assigned.empty()) {
                        for (auto* n : assigned) {
                            addNodeToParent(parent, n, childElem);
                        }
                    } else {
                        // Fallback: use slot's own children
                        for (auto* n : childElem->childNodes()) {
                            addNodeToParent(parent, n, childElem);
                        }
                    }
                    continue;
                }

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

    static void addNodeToParent(LayoutNodeAdapter* parent, dom::Node* node,
                                 dom::Element* contextElem) {
        if (node->nodeType() == dom::NodeType::Element) {
            auto* elem = static_cast<dom::Element*>(node);
            auto& style = elem->computedStyle();
            auto it = style.find("display");
            if (it != style.end() && it->second == "none") return;

            auto child = std::make_unique<LayoutNodeAdapter>(elem);
            child->parent_ = parent;
            buildChildren(child.get(), elem);
            parent->children_.push_back(std::move(child));
        } else if (node->nodeType() == dom::NodeType::Text) {
            auto* textNode = static_cast<dom::TextNode*>(node);
            if (textNode->data().empty()) return;
            auto child = std::make_unique<LayoutNodeAdapter>(textNode, contextElem);
            child->parent_ = parent;
            parent->children_.push_back(std::move(child));
        }
    }

    dom::Element* elem_ = nullptr;
    dom::TextNode* textNode_ = nullptr;
    dom::Element* parentElem_ = nullptr;  // for text nodes: their parent element
    LayoutNodeAdapter* parent_ = nullptr;
    std::vector<std::unique_ptr<LayoutNodeAdapter>> children_;

    // Synthetic ::before / ::after wrappers (only set on element-kind adapters
    // whose host has resolved pseudo content). The wrappers live outside
    // children_ so getLayoutChildren() can return them via pseudoBefore/After.
    // Mutable: ensurePseudo creates/destroys them on demand from a const
    // accessor when the host's pseudo content turns on or off between layouts.
    mutable std::unique_ptr<LayoutNodeAdapter> pseudoBeforeAdapter_;
    mutable std::unique_ptr<LayoutNodeAdapter> pseudoAfterAdapter_;

    // Pseudo-element identification (set on synthetic adapters only).
    dom::Element* pseudoHost_ = nullptr;
    std::string pseudoWhich_;       // "before" or "after"
    bool pseudoIsText_ = false;     // false: pseudo wrapper; true: pseudo's text child
};

} // namespace bro::layout
