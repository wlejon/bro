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
    class ElVideo;
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
    const std::string& id() const;
    void setId(const std::string& val);
    const std::string& className() const;
    void setClassName(const std::string& val);

    // Attributes
    const std::string& getAttribute(const std::string& name) const;
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

    // Resolve a relative URL against the owning Document's basePath.
    // Absolute paths are returned unchanged. Kept on Element so callers
    // that can't pull the full document.h (gumbo transitive include)
    // still have a convenient resolver — mirrors what Image and fetch do.
    std::string resolveUrl(const std::string& src) const;

    // Computed style (set by Cascade::resolve during style resolution)
    const htmlayout::css::ComputedStyle& computedStyle() const { return computedStyle_; }
    htmlayout::css::ComputedStyle& computedStyleMut() { return computedStyle_; }
    void setComputedStyle(htmlayout::css::ComputedStyle style) { computedStyle_ = std::move(style); }

    // Layout box (set by htmlayout::layout::layoutTree)
    const htmlayout::layout::LayoutBox& layoutBox() const { return layoutBox_; }
    void setLayoutBox(const htmlayout::layout::LayoutBox& box) { layoutBox_ = box; }

    // Generated content for ::before / ::after. The cascade resolves these
    // in Document::resolveStyles when CSS rules target the pseudo-element.
    // `which` is "before" or "after". Empty content string means no pseudo.
    const std::string& pseudoContent(const std::string& which) const {
        static const std::string empty;
        if (which == "before") return pseudoBeforeContent_;
        if (which == "after")  return pseudoAfterContent_;
        return empty;
    }
    // Whether a ::before/::after pseudo-element exists at all. A pseudo with
    // `content: ""` (empty string) still generates a box that participates in
    // layout (e.g. clearfix, block spacers), so box generation is gated on
    // this flag rather than on the content text being non-empty.
    bool hasPseudo(const std::string& which) const {
        if (which == "before") return pseudoBeforeActive_;
        if (which == "after")  return pseudoAfterActive_;
        return false;
    }
    const htmlayout::css::ComputedStyle& pseudoStyle(const std::string& which) const {
        static const htmlayout::css::ComputedStyle empty;
        if (which == "before") return pseudoBeforeStyle_;
        if (which == "after")  return pseudoAfterStyle_;
        return empty;
    }
    const htmlayout::layout::LayoutBox& pseudoBox(const std::string& which) const {
        static const htmlayout::layout::LayoutBox empty;
        if (which == "before") return pseudoBeforeBox_;
        if (which == "after")  return pseudoAfterBox_;
        return empty;
    }
    htmlayout::layout::LayoutBox& pseudoBoxMut(const std::string& which) {
        static htmlayout::layout::LayoutBox empty;
        if (which == "before") return pseudoBeforeBox_;
        if (which == "after")  return pseudoAfterBox_;
        return empty;
    }
    void setPseudo(const std::string& which,
                   std::string content,
                   htmlayout::css::ComputedStyle style) {
        if (which == "before") {
            pseudoBeforeContent_ = std::move(content);
            pseudoBeforeStyle_ = std::move(style);
            pseudoBeforeActive_ = true;
        } else if (which == "after") {
            pseudoAfterContent_ = std::move(content);
            pseudoAfterStyle_ = std::move(style);
            pseudoAfterActive_ = true;
        }
    }
    void clearPseudos() {
        pseudoBeforeContent_.clear();
        pseudoAfterContent_.clear();
        pseudoBeforeStyle_.clear();
        pseudoAfterStyle_.clear();
        pseudoBeforeActive_ = false;
        pseudoAfterActive_ = false;
    }

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
    void setScrollToBottom(bool v);

    // Replaced element controls
    layout::ElInput* inputControl() const { return inputControl_.get(); }
    layout::ElTextarea* textareaControl() const { return textareaControl_.get(); }
    layout::ElSelect* selectControl() const { return selectControl_.get(); }
    layout::ElSvg* svgControl() const { return svgControl_.get(); }
    layout::ElVideo* videoControl() const { return videoControl_.get(); }

    void setInputControl(std::unique_ptr<layout::ElInput> ctrl);
    void setTextareaControl(std::unique_ptr<layout::ElTextarea> ctrl);
    void setSelectControl(std::unique_ptr<layout::ElSelect> ctrl);
    void setSvgControl(std::unique_ptr<layout::ElSvg> ctrl);
    void setVideoControl(std::unique_ptr<layout::ElVideo> ctrl);

    // Canvas scene (opaque pointer — set by engine, read by draw traversal).
    // The optional onDestroy hook is invoked from ~Element so the backing
    // CanvasScene can drop its dangling pointer to this Element the instant it
    // is freed — including the deferred-free path (drainPendingFrees), which
    // destroys the Element without waiting for the JS wrapper to be GC'd.
    void setCanvasScene(void* scene, void (*onDestroy)(void*) = nullptr) {
        canvasScene_ = scene;
        canvasSceneOnDestroy_ = onDestroy;
    }
    void* canvasScene() const { return canvasScene_; }

    // WebGL context (opaque pointer — set by engine, read by draw traversal)
    void setWebglContext(void* ctx) { webglContext_ = ctx; }
    void* webglContext() const { return webglContext_; }

    // Scene graph (opaque pointer — set by engine, read by draw traversal for 3D FBO compositing)
    void setSceneGraph(void* graph) { sceneGraph_ = graph; }
    void* sceneGraph() const { return sceneGraph_; }

    // Iframe sub-document (opaque pointer — set by engine, read by the draw
    // traversal and input routing). Points at the engine's IframeDoc hosting
    // this <iframe>'s isolated sub-document. Null for a plain element.
    void setIframeDoc(void* d) { iframeDoc_ = d; }
    void* iframeDoc() const { return iframeDoc_; }

    // Scene graph mesh FBO texture (set by scene graph render, read by draw traversal)
    void setSceneGraphFBOTexture(unsigned int tex) { sceneGraphFBOTex_ = tex; }
    unsigned int sceneGraphFBOTexture() const { return sceneGraphFBOTex_; }

    // Custom validity message (HTMLInputElement.setCustomValidity). Empty
    // string means no custom error — any other value is treated as a
    // validation failure and returned by validationMessage.
    const std::string& customValidity() const { return customValidity_; }
    void setCustomValidity(const std::string& m) { customValidity_ = m; }

    // Debug: detect use-after-free
    bool isAlive() const { return magic_ == 0xB00E; }

private:
    std::string tag_;
    std::unordered_map<std::string, std::string> attributes_;
    StyleProxy style_;
    // "style" is never stored in attributes_ (its value lives in style_), so
    // presence has to be tracked separately: a style attribute set to an
    // empty/all-removed declaration block still exists (getAttribute returns
    // "", not null) until removeAttribute("style") is called.
    bool hasStyleAttr_ = false;
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

    // ::before / ::after generated content (empty = no pseudo)
    std::string pseudoBeforeContent_;
    std::string pseudoAfterContent_;
    htmlayout::css::ComputedStyle pseudoBeforeStyle_;
    htmlayout::css::ComputedStyle pseudoAfterStyle_;
    htmlayout::layout::LayoutBox pseudoBeforeBox_;
    htmlayout::layout::LayoutBox pseudoAfterBox_;
    bool pseudoBeforeActive_ = false;
    bool pseudoAfterActive_ = false;

    // Replaced element controllers
    std::unique_ptr<layout::ElInput> inputControl_;
    std::unique_ptr<layout::ElTextarea> textareaControl_;
    std::unique_ptr<layout::ElSelect> selectControl_;
    std::unique_ptr<layout::ElSvg> svgControl_;
    std::unique_ptr<layout::ElVideo> videoControl_;
    std::string customValidity_;
    void* canvasScene_ = nullptr;
    void (*canvasSceneOnDestroy_)(void*) = nullptr;
    void* webglContext_ = nullptr;
    void* sceneGraph_ = nullptr;
    void* iframeDoc_ = nullptr;
    unsigned int sceneGraphFBOTex_ = 0;
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
