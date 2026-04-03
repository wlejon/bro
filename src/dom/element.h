#pragma once
#include "dom/node.h"
#include "dom/shadow_root.h"
#include "dom/style_proxy.h"
#include "css/cascade.h"
#include "layout/box.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <cstdint>

namespace bro::layout {
    class ElInput;
    class ElTextarea;
    class ElSelect;
    class ElSvg;
}

namespace bro::dom {

class Document;
class TextNode;

class Element : public Node {
public:
    explicit Element(const std::string& tag);
    ~Element() override;

    NodeType nodeType() const override { return NodeType::Element; }
    std::string nodeName() const override { return tag_; }

    // Tag and identity
    const std::string& tagName() const { return tag_; }
    std::string id() const;
    void setId(const std::string& val);
    std::string className() const;
    void setClassName(const std::string& val);

    // Attributes
    std::string getAttribute(const std::string& name) const;
    bool hasAttribute(const std::string& name) const;
    void setAttribute(const std::string& name, const std::string& val);
    void removeAttribute(const std::string& name);
    const std::unordered_map<std::string, std::string>& attributes() const { return attributes_; }

    // Content
    std::string textContent() const;
    void setTextContent(const std::string& text);
    std::string innerHTML() const;
    std::string outerHTML() const;
    void setInnerHTML(const std::string& html);
    void setOuterHTML(const std::string& html);

    // Style
    StyleProxy& style() { return style_; }
    const StyleProxy& style() const { return style_; }

    // Event listeners
    void addEventListener(const std::string& type, uint64_t listenerId);
    void removeEventListener(const std::string& type, uint64_t listenerId);
    const std::unordered_map<std::string, std::vector<uint64_t>>& listeners() const { return listeners_; }

    // Tree traversal
    std::vector<Element*> children() const;
    Element* parentElement() const;

    // Composed tree traversal (shadow DOM + slot replacement).
    // Calls fn(Element*) for each composed child element, resolving shadow roots and slots.
    template<typename Fn>
    void forEachComposedChild(Fn&& fn) const;

    // Collect all composed child nodes into a flat vector (shadow DOM + slot replacement).
    // Includes both Element and non-Element nodes. Callers can iterate forward or reverse.
    std::vector<Node*> composedChildNodes() const;

    // Selectors (powered by htmlayout)
    std::vector<Element*> querySelectorAll(const std::string& selector);
    Element* querySelector(const std::string& selector);
    bool matches(const std::string& selector) const;
    Element* closest(const std::string& selector);

    // Simple selector matching (works for dynamic elements)
    bool matchesSimple(const std::string& selector) const;
    void querySelectorAllSimple(const std::string& selector, std::vector<Element*>& out);
    Element* querySelectorSimple(const std::string& selector);

    // Dirty tracking
    void markDirty();
    void markStructureDirty();
    bool isDirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

    // Owner document
    void setDocument(Document* doc) { document_ = doc; }
    Document* document() const { return document_; }

    // Computed style (set by Cascade::resolve during style resolution)
    const htmlayout::css::ComputedStyle& computedStyle() const { return computedStyle_; }
    htmlayout::css::ComputedStyle& computedStyleMut() { return computedStyle_; }
    void setComputedStyle(htmlayout::css::ComputedStyle style) { computedStyle_ = std::move(style); }

    // Layout box (set by htmlayout::layout::layoutTree)
    const htmlayout::layout::LayoutBox& layoutBox() const { return layoutBox_; }
    void setLayoutBox(const htmlayout::layout::LayoutBox& box) { layoutBox_ = box; }

    // Shadow DOM
    ShadowRoot* attachShadow(ShadowRoot::Mode mode);
    ShadowRoot* shadowRoot() const { return shadowRoot_; }
    bool hasShadow() const { return shadowRoot_ != nullptr; }

    // Returns the ShadowRoot this element is inside, or nullptr if not in shadow DOM
    ShadowRoot* containingShadowRoot() const;

    // Returns the layout parent of this element in the composed tree.
    // For slotted elements (light DOM children distributed through a <slot>),
    // this returns the element containing the slot, not the DOM parent.
    // For shadow DOM children, crosses the shadow boundary to the host.
    Element* layoutParent() const;

    // Element-level scroll offset (for overflow:auto/scroll elements)
    float scrollTopValue() const { return scrollTop_; }
    void setScrollTopValue(float v) { scrollTop_ = v; }

    // Auto-scroll flag: scroll to bottom after next layout
    bool needsScrollToBottom() const { return scrollToBottom_; }
    void setScrollToBottom(bool v) { scrollToBottom_ = v; }

    // Replaced element controls
    layout::ElInput* inputControl() const { return inputControl_.get(); }
    layout::ElTextarea* textareaControl() const { return textareaControl_.get(); }
    layout::ElSelect* selectControl() const { return selectControl_.get(); }
    layout::ElSvg* svgControl() const { return svgControl_.get(); }

    void setInputControl(std::unique_ptr<layout::ElInput> ctrl);
    void setTextareaControl(std::unique_ptr<layout::ElTextarea> ctrl);
    void setSelectControl(std::unique_ptr<layout::ElSelect> ctrl);
    void setSvgControl(std::unique_ptr<layout::ElSvg> ctrl);

    // Debug: detect use-after-free
    bool isAlive() const { return magic_ == 0xB00E; }

private:
    std::string tag_;
    std::unordered_map<std::string, std::string> attributes_;
    StyleProxy style_;
    std::unordered_map<std::string, std::vector<uint64_t>> listeners_;
    Document* document_ = nullptr;
    ShadowRoot* shadowRoot_ = nullptr;
    bool dirty_ = false;
    bool scrollToBottom_ = false;
    float scrollTop_ = 0.0f;
    uint32_t magic_ = 0xB00E;

    // htmlayout integration
    htmlayout::css::ComputedStyle computedStyle_;
    htmlayout::layout::LayoutBox layoutBox_;

    // Replaced element controllers
    std::unique_ptr<layout::ElInput> inputControl_;
    std::unique_ptr<layout::ElTextarea> textareaControl_;
    std::unique_ptr<layout::ElSelect> selectControl_;
    std::unique_ptr<layout::ElSvg> svgControl_;
};

// Template implementation — must be in header
template<typename Fn>
void Element::forEachComposedChild(Fn&& fn) const {
    for (auto* node : composedChildNodes()) {
        if (node->nodeType() == NodeType::Element)
            fn(static_cast<Element*>(node));
    }
}

} // namespace bro::dom
