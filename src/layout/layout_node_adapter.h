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
#include <string_view>
#include <span>
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

    std::string_view tagName() const override {
        if (pseudoHost_) return pseudoIsText_ ? std::string_view{"#text"} : std::string_view{pseudoTag_};
        if (elem_) return std::string_view{elem_->tagName()};
        return std::string_view{"#text"};
    }

    bool isTextNode() const override {
        if (pseudoHost_) return pseudoIsText_;
        return textNode_ != nullptr;
    }

    std::string_view textContent() const override {
        if (pseudoHost_ && pseudoIsText_) return std::string_view{pseudoHost_->pseudoContent(pseudoWhich_)};
        if (textNode_) return std::string_view{textNode_->data()};
        return {};
    }

    // HTML attribute lookup for layout-affecting presentational attributes
    // (colspan/rowspan/span on table parts). getAttribute returns a reference
    // into the element's attribute map (or a static empty string), so the
    // view outlives the call as the interface requires.
    std::string_view attribute(std::string_view name) const override {
        if (!elem_) return {};
        return elem_->getAttribute(std::string(name));
    }

    LayoutNode* parent() const override { return parent_; }

    std::span<LayoutNode* const> children() const override {
        childrenView_.clear();
        childrenView_.reserve(children_.size());
        for (auto& child : children_) {
            childrenView_.push_back(child.get());
        }
        return std::span<LayoutNode* const>{childrenView_.data(), childrenView_.size()};
    }

    const htmlayout::css::ComputedStyle& computedStyle() const override {
        if (pseudoHost_) return pseudoHost_->pseudoStyle(pseudoWhich_);
        if (elem_) return elem_->computedStyle();
        // Text nodes inherit parent's style
        if (parentElem_) return parentElem_->computedStyle();
        static const htmlayout::css::ComputedStyle empty;
        return empty;
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
        // <iframe> is a replaced element hosting a sub-document. Its box is set
        // by CSS width/height (or the width/height attrs), falling back to the
        // HTML default 300x150. Its own children are not laid out by the host
        // tree (see buildChildren) — the sub-document renders into this box.
        if (elem_->tagName() == "iframe" || elem_->tagName() == "IFRAME") {
            std::string wa = elem_->getAttribute("width");
            std::string ha = elem_->getAttribute("height");
            w = wa.empty() ? 300.0f : std::strtof(wa.c_str(), nullptr);
            h = ha.empty() ? 150.0f : std::strtof(ha.c_str(), nullptr);
            return true;
        }
        // <img> is a replaced element. Resolve intrinsic dimensions from:
        //   1. width/height HTML attributes (presentational hints)
        //   2. SVG data URL: parse <svg width=... height=...> from the src
        //   3. aspect-ratio derivation when only one axis is specified
        if (elem_->tagName() == "img" || elem_->tagName() == "IMG") {
            std::string wa = elem_->getAttribute("width");
            std::string ha = elem_->getAttribute("height");
            float attrW = wa.empty() ? -1.0f : std::strtof(wa.c_str(), nullptr);
            float attrH = ha.empty() ? -1.0f : std::strtof(ha.c_str(), nullptr);
            float intrW = -1.0f, intrH = -1.0f;
            std::string src = elem_->getAttribute("src");
            // Extract intrinsic w/h from an SVG data URL by string-parsing the
            // outer <svg ...> width/height attributes. We don't render the SVG
            // here; we just need its declared intrinsic size for layout.
            if (src.rfind("data:image/svg+xml", 0) == 0) {
                auto pos = src.find(',');
                if (pos != std::string::npos) {
                    std::string body = src.substr(pos + 1);
                    auto svgPos = body.find("<svg");
                    if (svgPos != std::string::npos) {
                        auto end = body.find('>', svgPos);
                        if (end != std::string::npos) {
                            std::string tag = body.substr(svgPos, end - svgPos);
                            auto extractAttr = [&](const char* name) -> float {
                                std::string needle = std::string(" ") + name + "=";
                                auto p = tag.find(needle);
                                if (p == std::string::npos) return -1.0f;
                                p += needle.size();
                                if (p >= tag.size()) return -1.0f;
                                char quote = tag[p];
                                if (quote != '"' && quote != '\'') return -1.0f;
                                p++;
                                auto endQ = tag.find(quote, p);
                                if (endQ == std::string::npos) return -1.0f;
                                std::string v = tag.substr(p, endQ - p);
                                return std::strtof(v.c_str(), nullptr);
                            };
                            intrW = extractAttr("width");
                            intrH = extractAttr("height");
                        }
                    }
                }
            }
            // Resolve final intrinsic size:
            //   - Explicit attrs on both axes win.
            //   - Single attr derives the other axis from intrinsic aspect ratio.
            //   - Otherwise use the SVG's intrinsic dimensions if known.
            float aspect = (intrW > 0 && intrH > 0) ? (intrW / intrH) : 0.0f;
            if (attrW >= 0 && attrH >= 0) { w = attrW; h = attrH; return true; }
            if (attrW >= 0) {
                w = attrW;
                h = (aspect > 0) ? (attrW / aspect) : (intrH > 0 ? intrH : 0.0f);
                return true;
            }
            if (attrH >= 0) {
                h = attrH;
                w = (aspect > 0) ? (attrH * aspect) : (intrW > 0 ? intrW : 0.0f);
                return true;
            }
            if (intrW > 0 || intrH > 0) {
                w = intrW > 0 ? intrW : 0.0f;
                h = intrH > 0 ? intrH : 0.0f;
                return true;
            }
            return false;
        }
        return false;
    }

    // Replaced *media* carry a fixed intrinsic aspect ratio: when one axis is
    // constrained (e.g. max-width:100% on a canvas), the other scales to match.
    // Form controls (input/textarea/select) report an intrinsic size but no
    // locked ratio, so they're excluded here.
    bool hasIntrinsicRatio() const override {
        if (!elem_) return false;
        std::string_view tag = elem_->tagName();
        return tag == "img" || tag == "IMG" || tag == "canvas" || tag == "CANVAS" ||
               tag == "video" || tag == "VIDEO" || tag == "svg" || tag == "SVG";
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
        : pseudoHost_(host), pseudoWhich_(which), pseudoTag_("::" + which), pseudoIsText_(isText) {}

    // Lazily build / tear down the synthetic pseudo wrapper on each layout
    // pass. Pseudo content can appear or disappear when classes change (e.g.
    // .selected moving between menu items toggles ::before/::after rules);
    // checking elem_->pseudoContent at access time keeps the layout tree in
    // sync without requiring a structural rebuild.
    LayoutNode* ensurePseudo(const std::string& which) const {
        if (!elem_) return nullptr;
        bool want = elem_->hasPseudo(which);
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

        // <select> and <textarea> are widget-replaced: their children
        // (<option>s, the initial text) belong to the control, not the
        // document flow. The control reads them directly from the DOM
        // (ElSelect::getOptions / ElTextarea::readCurrentValue), so they
        // must not become layout boxes — otherwise a grid/flex/block that
        // doesn't short-circuit replaced elements would lay the options out
        // as visible blocks. Their computed style still resolves (matching
        // the browser's `option { display: block }`), but they stay 0×0.
        // Match on tag, not the ElSelect/ElTextarea control pointer: the
        // control is attached lazily (replaced_elements.cpp) and may still be
        // null on the layout pass that first builds this subtree.
        {
            std::string_view tag = elem->tagName();
            if (tag == "select" || tag == "SELECT" ||
                tag == "textarea" || tag == "TEXTAREA") return;
            // <iframe> hosts an isolated sub-document rendered into its box; its
            // markup children (fallback content) are not laid out by the host.
            if (tag == "iframe" || tag == "IFRAME") return;
        }

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

                // Include display:none subtrees in the layout tree.
                // layoutNode() zero-sizes them and skips children, and the
                // various layout containers (block, flex, grid, table)
                // re-check display dynamically before measuring children.
                // Skipping at build time would mean a `display:none` →
                // `display:block` transition (e.g. a collapsible panel
                // expanding) leaves the subtree without LayoutNodes — its
                // computed style updates correctly but layout never runs,
                // so descendants stay 0×0 and absolutely-positioned ones
                // resolve their containing block up past the now-orphan
                // ancestors to the viewport, painting full-width streaks.
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
    mutable std::vector<LayoutNode*> childrenView_;

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
    std::string pseudoTag_;         // "::before" or "::after" (stable backing for tagName view)
    bool pseudoIsText_ = false;     // false: pseudo wrapper; true: pseudo's text child
};

} // namespace bro::layout
