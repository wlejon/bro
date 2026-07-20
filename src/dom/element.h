#pragma once
#include "dom/node.h"
#include "dom/shadow_root.h"
#include "dom/style_proxy.h"
#include "dom/string_flat_map.h"
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

// ---------------------------------------------------------------------------
// Debug-only use-after-free tripwire for Element storage.
//
// isAlive() cannot be used to VALIDATE a pointer: magic_ is stamped 0xDEAD by
// ~Element, so any code that reads it to find out whether an Element is still
// there has already read freed memory. Such a read is silent in practice (the
// freed page usually still holds 0xDEAD, so the probe even returns the "right"
// answer) which is exactly what let this class of bug hide.
//
// In Debug builds every ~Element records its address here and every Element
// constructor removes its own address again — so the set is precisely "storage
// that was an Element and has since been destroyed, and has not been reused by
// a newer Element". Call sites that must never see a freed Element assert on
// it via BRO_ASSERT_ELEMENT_NOT_FREED, which reports and exits rather than
// letting the read pass unnoticed.
//
// JS-thread only, like all Element construction/destruction.
#ifndef NDEBUG
#define BRO_DOM_FREED_ELEMENT_GUARD 1
bool wasElementFreed(const void* p);
// Reports and terminates. `what` names the call site.
[[noreturn]] void reportFreedElementAccess(const void* p, const char* what);
#define BRO_ASSERT_ELEMENT_NOT_FREED(p, what)                                  \
    do {                                                                       \
        if ((p) && ::bro::dom::wasElementFreed(p))                             \
            ::bro::dom::reportFreedElementAccess((p), (what));                 \
    } while (0)
#else
#define BRO_ASSERT_ELEMENT_NOT_FREED(p, what) ((void)0)
#endif

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
    const StringFlatMap& attributes() const { return attributes_; }

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
    // Like markDirty(), but only requests a re-record (paint), not a full
    // re-layout — used by the hover restyle. See Document::markPaintDirty.
    void markPaintDirty();
    // An inline-style write. Paint-dirty like markPaintDirty(), but it does NOT
    // set selectorDirty_: inline style is not a selector input, so no descendant
    // can start or stop matching a rule because of it. Descendants are re-resolved
    // only if this element's *inherited* values actually changed — which is what
    // keeps `container.style.opacity = x` off the whole subtree's restyle bill.
    void markStyleDirty();
    // This element's child list changed. Its layout node's children get rebuilt
    // from the DOM on the next pass; the rest of the tree keeps its geometry.
    void markStructureDirty();
    bool isDirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

    // Read (and cleared) once per pass by the layout tree, which rebuilds this
    // element's layout children from the DOM. Also cleared by the tree builder
    // itself: a subtree it just built from the DOM cannot also be pending a
    // rebuild from the DOM.
    bool takeStructureDirty() { bool d = structureDirty_; structureDirty_ = false; return d; }

    // This element's own geometry may have changed — its style diff turned up a
    // layout-affecting property, an attribute or its text was rewritten. Read
    // (and cleared) once per pass by the layout tree, which dirties this
    // element's layout node and the ancestor chain above it so layout
    // recomputes that chain and reuses everything else. Separate from dirty_:
    // a hover that only repainted a background sets dirty_ and not this.
    void markLayoutDirty() { layoutDirty_ = true; }
    bool takeLayoutDirty() { bool d = layoutDirty_; layoutDirty_ = false; return d; }

    // A selector *input* on this element changed — class, id or an attribute —
    // so rules matching anywhere in its subtree (`.dark .btn`) may now match
    // differently and the whole subtree has to re-resolve. An inline-style write
    // sets dirty_ without this, and then only descendants that inherit a changed
    // value re-resolve. True initially: nothing is resolved yet.
    bool takeSelectorDirty() { bool d = selectorDirty_; selectorDirty_ = false; return d; }

    // A :hover flipped somewhere at or under this element. Like selectorDirty_
    // it means "rules may match differently below me", but the *only* input that
    // moved is :hover — so instead of re-resolving the subtree, each descendant
    // is asked whether a hover-descendant rule could name it (Cascade::
    // hoverCanAffect). Set on the hovered chain's common ancestor, because a
    // rule can reach a sibling (`.tab:hover + .panel`) as easily as a child.
    void markHoverScopeDirty();
    bool takeHoverScopeDirty() { bool d = hoverScopeDirty_; hoverScopeDirty_ = false; return d; }

    // Owner document — storage and accessors live on Node (see node.h).

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
    //
    // The backing storage (two ComputedStyle maps + two LayoutBoxes + two
    // content strings) is heavy and idle on the vast majority of elements — a
    // ::before/::after is rare — so it lives behind a lazily-allocated
    // PseudoData pointer instead of inline. An element with no pseudo pays one
    // null pointer; the block is allocated on first setPseudo() and freed when
    // both pseudos are cleared. Accessors return references to shared static
    // empties when the block is absent, matching the old inline behaviour.
    const std::string& pseudoContent(const std::string& which) const {
        static const std::string empty;
        if (!pseudo_) return empty;
        if (which == "before") return pseudo_->beforeContent;
        if (which == "after")  return pseudo_->afterContent;
        return empty;
    }
    // Whether a ::before/::after pseudo-element exists at all. A pseudo with
    // `content: ""` (empty string) still generates a box that participates in
    // layout (e.g. clearfix, block spacers), so box generation is gated on
    // this flag rather than on the content text being non-empty.
    bool hasPseudo(const std::string& which) const {
        if (!pseudo_) return false;
        if (which == "before") return pseudo_->beforeActive;
        if (which == "after")  return pseudo_->afterActive;
        return false;
    }
    const htmlayout::css::ComputedStyle& pseudoStyle(const std::string& which) const {
        static const htmlayout::css::ComputedStyle empty;
        if (!pseudo_) return empty;
        if (which == "before") return pseudo_->beforeStyle;
        if (which == "after")  return pseudo_->afterStyle;
        return empty;
    }
    const htmlayout::layout::LayoutBox& pseudoBox(const std::string& which) const {
        static const htmlayout::layout::LayoutBox empty;
        if (!pseudo_) return empty;
        if (which == "before") return pseudo_->beforeBox;
        if (which == "after")  return pseudo_->afterBox;
        return empty;
    }
    htmlayout::layout::LayoutBox& pseudoBoxMut(const std::string& which) {
        static htmlayout::layout::LayoutBox empty;
        if (which != "before" && which != "after") return empty;
        ensurePseudo();
        return (which == "before") ? pseudo_->beforeBox : pseudo_->afterBox;
    }
    void setPseudo(const std::string& which,
                   std::string content,
                   htmlayout::css::ComputedStyle style) {
        if (which != "before" && which != "after") return;
        ensurePseudo();
        if (which == "before") {
            pseudo_->beforeContent = std::move(content);
            pseudo_->beforeStyle = std::move(style);
            pseudo_->beforeActive = true;
        } else {
            pseudo_->afterContent = std::move(content);
            pseudo_->afterStyle = std::move(style);
            pseudo_->afterActive = true;
        }
    }
    void clearPseudos() {
        clearPseudo("before");
        clearPseudo("after");
    }
    void clearPseudo(const std::string& which) {
        if (!pseudo_) return;
        if (which == "before") {
            pseudo_->beforeContent.clear();
            pseudo_->beforeStyle.clear();
            pseudo_->beforeActive = false;
        } else if (which == "after") {
            pseudo_->afterContent.clear();
            pseudo_->afterStyle.clear();
            pseudo_->afterActive = false;
        }
        // Release the block once neither pseudo is live, so a transient pseudo
        // does not leave the heap footprint attached to the element forever.
        if (!pseudo_->beforeActive && !pseudo_->afterActive)
            pseudo_.reset();
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

    // For a <style> element: whether its CSS has been added to the document
    // cascade yet. Lets the document add a dynamically-inserted <style>'s rules
    // exactly once. The flag lives on the element so it can't outlive it (no
    // stale pointers) and survives detach/re-attach (rules aren't re-added).
    bool styleSheetAdded() const { return styleSheetAdded_; }
    void setStyleSheetAdded(bool v) { styleSheetAdded_ = v; }

    // Replaced element controls
    layout::ElInput* inputControl() const { return inputControl_.get(); }
    layout::ElTextarea* textareaControl() const { return textareaControl_.get(); }
    layout::ElSelect* selectControl() const { return selectControl_.get(); }
    layout::ElSvg* svgControl() const { return svgControl_.get(); }
    layout::ElVideo* videoControl() const { return videoControl_.get(); }

    // <img> intrinsic size — the decoded pixel dimensions of `src`.
    //
    // An <img> is a REPLACED element, and layout can only treat it as one if
    // it can answer "how big is it?". Without an answer it degenerates to an
    // empty inline box: zero-sized, and CSS width/height silently ignored
    // (correctly — those don't apply to non-replaced inlines), so the image
    // simply doesn't appear. Attributes and SVG data URLs used to be the only
    // sources of an answer, which is why a plain <img src="photo.png"> was
    // invisible.
    //
    // Filled in by engine::ensureReplacedElements, which resolves `src`
    // against the document base path and reads just the file header. Cached
    // against the src it was probed for so a src change re-probes, and a
    // layout pass never does I/O.
    int imageNaturalWidth() const { return imageNaturalWidth_; }
    int imageNaturalHeight() const { return imageNaturalHeight_; }
    /// The src these dimensions were probed for; empty until one is.
    const std::string& imageProbedSrc() const { return imageProbedSrc_; }
    void setImageNaturalSize(const std::string& src, int w, int h) {
        imageProbedSrc_ = src;
        imageNaturalWidth_ = w;
        imageNaturalHeight_ = h;
    }

    // Pre-layout <select> selection. selectedIndex is a DOM property scripts
    // read and write before any frame, but the ElSelect control that stores it
    // is not created until the first layout pass. This carries a value set
    // before then; ElSelect::initSelectedIndex() adopts it when the control is
    // created. Without it, a selectedIndex assignment before layout is silently
    // lost — the setter no-ops and the getter reads back -1.
    bool hasPendingSelectedIndex() const { return pendingSelIndexSet_; }
    int  pendingSelectedIndex() const { return pendingSelIndex_; }
    void setPendingSelectedIndex(int i) { pendingSelIndex_ = i; pendingSelIndexSet_ = true; }

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

    // Cached JS wrapper (opaque JSObject*, set by the binding layer). The DOM
    // holds a plain pointer with no ownership: the binding's element→wrapper map
    // keeps a strong reference to every cached wrapper, so a non-null value here
    // always points at a live wrapper. The wrapper's finalizer nulls this the
    // instant it is collected, and the detach/free paths clear it eagerly, so it
    // never dangles. Lets wrapElement() skip the per-crossing global lookup +
    // itoa + two hash lookups that dominated DOM traversal from JS.
    void* jsWrapper() const { return jsWrapper_; }
    void setJsWrapper(void* w) { jsWrapper_ = w; }

    // <template>.content — the parsed children of a <template>, held in a
    // DocumentFragment that is NOT part of the tree, so they never render and
    // never match a document query. Per spec the fragment is stable: the same
    // object every time .content is read, so mutations to it stick. Null on
    // every element that is not a <template>, and on a <template> whose content
    // has not been materialized yet.
    Element* templateContent() const { return templateContent_; }
    void setTemplateContent(Element* frag) {
        templateContent_ = frag;
        if (frag) frag->isTemplateContent_ = true;
    }

    // True on the fragment side of the link above. The orphan sweep frees any
    // parentless, childless #DOCUMENT-FRAGMENT it finds a wrapper for; a
    // <template> with no children owns exactly such a fragment, and it must
    // survive because the template still points at it.
    bool isTemplateContent() const { return isTemplateContent_; }

    // Debug: detect use-after-free
    //
    // NOTE: magic_ is stamped 0xDEAD by ~Element and nowhere else, so a false
    // return does not mean "live element in a bad state" — it means the storage
    // is ALREADY FREED and reading magic_ was itself a use-after-free. Never
    // call this to decide whether a pointer is safe to dereference; use
    // Document::ownsNode / isNodeLive / resolveNode, which never deref.
    bool isAlive() const { return magic_ == 0xB00E; }

private:
    std::string tag_;
    StringFlatMap attributes_;
    StyleProxy style_;
    // "style" is never stored in attributes_ (its value lives in style_), so
    // presence has to be tracked separately: a style attribute set to an
    // empty/all-removed declaration block still exists (getAttribute returns
    // "", not null) until removeAttribute("style") is called.
    bool hasStyleAttr_ = false;
    std::unordered_map<std::string, std::vector<uint64_t>> listeners_;
    ShadowRoot* shadowRoot_ = nullptr;
    Element* templateContent_ = nullptr;
    bool isTemplateContent_ = false;
    bool dirty_ = false;
    bool layoutDirty_ = false;
    bool selectorDirty_ = true;
    bool hoverScopeDirty_ = false;
    bool structureDirty_ = false;
    bool scrollToBottom_ = false;
    bool styleSheetAdded_ = false;
    float scrollTop_ = 0.0f;
    uint32_t magic_ = 0xB00E;

    // htmlayout integration
    htmlayout::css::ComputedStyle computedStyle_;
    htmlayout::layout::LayoutBox layoutBox_;

    // ::before / ::after generated content, lazily allocated (see accessors).
    // Null until an element actually has a pseudo-element resolved.
    struct PseudoData {
        std::string beforeContent;
        std::string afterContent;
        htmlayout::css::ComputedStyle beforeStyle;
        htmlayout::css::ComputedStyle afterStyle;
        htmlayout::layout::LayoutBox beforeBox;
        htmlayout::layout::LayoutBox afterBox;
        bool beforeActive = false;
        bool afterActive = false;
    };
    std::unique_ptr<PseudoData> pseudo_;
    void ensurePseudo() { if (!pseudo_) pseudo_ = std::make_unique<PseudoData>(); }

    // Replaced element controllers
    std::unique_ptr<layout::ElInput> inputControl_;
    std::unique_ptr<layout::ElTextarea> textareaControl_;
    std::unique_ptr<layout::ElSelect> selectControl_;
    // selectedIndex set before the layout control exists — see the accessors.
    int  pendingSelIndex_ = 0;
    bool pendingSelIndexSet_ = false;
    std::unique_ptr<layout::ElSvg> svgControl_;
    // <img> probed intrinsic size — see imageNaturalWidth().
    std::string imageProbedSrc_;
    int imageNaturalWidth_ = 0;
    int imageNaturalHeight_ = 0;
    std::unique_ptr<layout::ElVideo> videoControl_;
    std::string customValidity_;
    void* canvasScene_ = nullptr;
    void (*canvasSceneOnDestroy_)(void*) = nullptr;
    void* webglContext_ = nullptr;
    void* sceneGraph_ = nullptr;
    void* iframeDoc_ = nullptr;
    void* jsWrapper_ = nullptr;
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

// Serialize an <svg> subtree for the SkSVGDOM fallback renderer: renames
// `href` to `xlink:href` on referencing elements (Skia parses only the xlink
// form) and expands <use> of a <symbol> into an inline <svg> viewport (Skia
// has no symbol node). See element.cpp for details.
std::string serializeSvgForRenderer(const Element* svgRoot);

} // namespace bro::dom
