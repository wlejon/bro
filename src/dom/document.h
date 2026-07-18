#pragma once
#include "dom/node.h"
#include "dom/element.h"
#include "dom/text_node.h"
#include "dom/comment_node.h"
#include "dom/document_fragment.h"
#include "dom/shadow_root.h"
#include "css/cascade.h"
#include "layout/box.h"
#include <gumbo.h>
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>

namespace bro::layout { class LayoutNodeAdapter; }
namespace bro::render { class Renderer; }
namespace bro::engine { class TransitionManager; class AnimationManager; class WebAnimationManager; }

namespace bro::dom {

class Range;
class Selection;

class Document {
public:
    Document();
    ~Document();

    // Parsing — parse HTML with gumbo, extract <style> CSS, build DOM tree
    // uaCss: user-agent default styles (lowest priority in cascade)
    // authorCss: app/user stylesheets (normal author priority)
    void parse(const std::string& html, const std::string& authorCss = {},
               const std::string& uaCss = {});

    // Node creation — Document owns all nodes via ownedNodes_.
    Element* createElement(const std::string& tag);
    TextNode* createTextNode(const std::string& text);
    CommentNode* createComment(const std::string& data);
    DocumentFragment* createDocumentFragment();

    // Allocate a ShadowRoot owned by this document
    ShadowRoot* allocateShadowRoot(Element* host, ShadowRoot::Mode mode);

    // Queue a node for deletion. The node is detached from ownedNodes_ and
    // held in pendingFrees_ until drainPendingFrees() runs. Deferral is
    // required because the raster/layout threads may still hold pointers
    // into the DOM from an in-flight traversal — freeing synchronously
    // while they read would crash. Called after removal from tree + JS
    // wrapper invalidation.
    void freeNode(Node* node);

    // Destroy any nodes queued by freeNode(). Caller must guarantee no
    // other thread is reading the DOM (layout + raster both idle).
    void drainPendingFrees();

    // Visit every Element whose memory is still alive in this document — both
    // owned nodes and nodes queued in pendingFrees_ (with their subtrees).
    //
    // Engine teardown uses this to sever Element->CanvasScene back-pointers
    // before destroying the scenes. Severing from the scene side instead
    // (scene->backingElement()) is NOT sufficient: the link is not reliably
    // 1:1 — an Element can hold a scene that the scene no longer names — so
    // that approach leaves stale pointers which ~Element then dereferences.
    // May visit an Element more than once; callers must be idempotent.
    void forEachLiveElement(const std::function<void(Element*)>& fn);

    // True if `n` is still a node owned by this document (i.e. present in
    // ownedNodes_ and not queued for deferred free). Use this to validate
    // raw Node* pointers held across frames — e.g. Range endpoints, cached
    // hit-test results — before dereferencing them.
    bool ownsNode(const Node* n) const;

    // True if `n` points at a node whose memory is still alive — either owned
    // (in the tree / offscreen) or queued in pendingFrees_ but not yet drained.
    // Both checks are pointer-value lookups, so this is safe to call on a
    // possibly-dangling pointer: it never dereferences `n`. CanvasScene uses it
    // to decide whether its raw backing-Element pointer is safe to touch.
    bool isNodeLive(const Node* n) const;

    // Generation-checked resolve backing NodeHandle: returns `ptr` iff its
    // memory is still alive in this document (owned or pending free) AND the
    // node at that address still carries nodeId `id`. Safe on dangling
    // pointers (pointer-value lookup before any dereference), and immune to
    // address reuse because node ids are never recycled.
    Node* resolveNode(const Node* ptr, uint32_t id) const;

    // True if `doc` points at a live Document. Documents register/unregister
    // in ctor/dtor (main thread only), letting NodeHandle survive its whole
    // document being destroyed (e.g. a closed system panel).
    static bool isLiveDocument(const Document* doc);

    // Queries
    Element* getElementById(const std::string& id);
    Element* querySelector(const std::string& selector);
    std::vector<Element*> querySelectorAll(const std::string& selector);

    // Tree accessors
    Element* body() const { return body_; }
    Element* documentElement() const { return documentElement_; }

    // Focus tracking
    Element* activeElement() const { return focusedElement_ ? focusedElement_ : body_; }
    void setActiveElement(Element* el);

    // Title
    std::string title() const;
    void setTitle(const std::string& title);

    // Dirty tracking. Two levels:
    //  - dirty_       : the frame needs a re-record (paint). Set by everything.
    //  - layoutDirty_ : the frame also needs a full layoutTree() pass. Set by
    //                   markDirty() (the conservative default — any change might
    //                   move geometry) and markStructureDirty(), but NOT by
    //                   markPaintDirty(). The hover restyle uses markPaintDirty
    //                   because :hover rules are almost always paint-only
    //                   (background/color); if a :hover rule DOES change a
    //                   layout-affecting property, resolveStyles() detects it via
    //                   the computed-style diff and calls promoteLayoutDirty(),
    //                   so geometry stays correct. This is what keeps hover (and
    //                   any paint-only restyle) off the O(N) re-layout that
    //                   otherwise runs on every mouse move.
    //
    // Layout is incremental (htmlayout::layout::layoutTree reuses the geometry
    // of any subtree that didn't change), so the pass also needs to know *what*
    // changed. Element::markDirty() records that on the element and calls
    // markElementDirty() here; Document::markDirty() is the unattributed
    // version — the caller can't name an element, so the whole tree is
    // relaid-out. Prefer the element form wherever there is an element.
    void markDirty() { dirty_ = true; layoutDirty_ = true; fullLayout_ = true; }
    void markElementDirty() { dirty_ = true; layoutDirty_ = true; }
    void markPaintDirty() { dirty_ = true; }
    // An inline style wrote a bare `inherit`. We do not know here whether the
    // property inherits, so take the safe branch and stop scoping restyles.
    void noteForcedInherit() { forcedInherit_ = true; }

    // Did this class change touch a class that some selector names in an
    // ancestor position — i.e. can it re-match this element's descendants?
    // Only the classes that were actually added or removed are asked; a rewrite
    // of the same set changes nothing.
    bool classChangeAffectsDescendants(const std::string& oldCls,
                                       const std::string& newCls) const;
    void promoteLayoutDirty() { layoutDirty_ = true; }
    bool isDirty() const { return dirty_; }
    bool isLayoutDirty() const { return layoutDirty_; }
    void clearDirty() { dirty_ = false; layoutDirty_ = false; }

    // Structure dirty — DOM nodes were added or removed somewhere this frame.
    // Consumers read isStructureDirty() to re-scan for things that follow the
    // DOM's shape (replaced-element controls, iframe documents).
    //
    // Also arms a style-element reconcile: a subtree that just changed may have
    // brought in (or repopulated) a <style>, so resolveStyles re-scans for any
    // whose CSS isn't in the cascade yet.
    //
    // This is the element-attributed form: Element::markStructureDirty() has
    // already recorded *which* element's children moved, so the layout tree
    // rebuilds that node's children and keeps every other subtree's geometry.
    void markElementStructureDirty() {
        structureDirty_ = true; dirty_ = true; layoutDirty_ = true; styleElsDirty_ = true;
    }
    // ...and this is the unattributed form, for a change no element can be
    // pinned to: a reparse, a fresh documentElement. Throw the layout tree away
    // and rebuild it whole. Prefer the element form wherever there is an element
    // — on a few-thousand-element document this one costs ~150ms.
    void markStructureDirty() {
        markElementStructureDirty();
        rebuildLayoutTree_ = true;
        fullLayout_ = true;
    }
    bool isStructureDirty() const { return structureDirty_; }
    void clearStructureDirty() { structureDirty_ = false; }

    // CSS cascade — add stylesheets, resolve styles
    htmlayout::css::Cascade& cascade() { return cascade_; }

    // Resolve computed styles for all elements in the tree
    void resolveStyles();

    // Set transition/animation managers (optional, set by Engine).
    void setTransitionManager(engine::TransitionManager* tm, double time) {
        transitionManager_ = tm;
        transitionTime_ = time;
    }
    void setAnimationManager(engine::AnimationManager* am) {
        animationManager_ = am;
    }
    void setWebAnimationManager(engine::WebAnimationManager* wam) {
        webAnimationManager_ = wam;
    }

    // Perform layout on the tree using htmlayout
    void performLayout(float viewportWidth, htmlayout::layout::TextMetrics& metrics);
    void performLayout(float viewportWidth, float viewportHeight, htmlayout::layout::TextMetrics& metrics);

    // What the style + layout passes have cost since the last reset. Accumulates
    // across passes so a caller can bracket a whole scenario (a drag, a frame, a
    // hundred flushes) and read one total. Surfaced by headless `perf.stats()`.
    //
    // The counts matter more than the milliseconds. `nodesLaidOut` against
    // `nodesReused` says whether the incremental layout is working at all, and
    // `measureCalls` says whether the cost is text shaping — a pass that changes
    // one element and lays out four thousand nodes has an invalidation bug, not
    // a speed problem, and only the counts show that.
    struct Perf {
        double styleMs = 0;        // resolveStyles(): cascade + computed-style diff
        double buildMs = 0;        // rebuilding the whole layout tree from the DOM
        double invalidateMs = 0;   // carrying element dirt in, incl. per-element rebuilds
        double layoutMs = 0;       // htmlayout::layout::layoutTree(), which is:
        double layoutTreeMs = 0;   //   in-flow layout (the incremental part)
        double layoutAbsMs = 0;    //   positioning absolute/fixed boxes
        double layoutHitMs = 0;    //   caching per-node subtree hit bounds
        double syncMs = 0;         // writing boxes back onto elements
        uint64_t passes = 0;       // performLayout() calls
        uint64_t treeRebuilds = 0; // layout subtrees rebuilt from the DOM
        uint64_t elementsStyled = 0;
        uint64_t nodesLaidOut = 0;
        uint64_t nodeVisits = 0;
        uint64_t nodesReused = 0;
        uint64_t measureCalls = 0;
        uint64_t styleLookups = 0; // styleVal() map lookups inside layoutTree()
        // Why nodes were relaid rather than reused, by first failing condition:
        // real dirt vs. a changed layout input cascading down a clean subtree.
        uint64_t reuseFailDirty = 0;
        uint64_t reuseFailAvailW = 0;
        uint64_t reuseFailAvailH = 0;
        uint64_t reuseFailOverride = 0;
        double totalMs() const { return styleMs + buildMs + invalidateMs + layoutMs + syncMs; }
    };
    const Perf& perf() const { return perf_; }
    void resetPerf() { perf_ = {}; }

    // Persistent layout-node tree. Rebuilt when structureDirty_ is set (DOM
    // mutations, shadow/slot changes, display toggles); reused across layouts
    // when only styles/sizes change. Hit testing walks this tree directly.
    layout::LayoutNodeAdapter* layoutRoot() const { return layoutRoot_.get(); }

    // ID map management (called by elements when id attribute changes)
    void registerElementId(const std::string& id, Element* elem);
    void unregisterElementId(const std::string& id);

    /// Pre-process HTML: extract <template> blocks that gumbo would
    /// discard, replacing them with hidden placeholder divs.
    struct TemplateBlock {
        std::string id;
        std::string attrs;
        std::string innerHTML;
    };
    static std::string extractTemplates(const std::string& html,
                                        std::vector<TemplateBlock>& out);

    /// After parse(), call this to populate placeholder elements with
    /// their template content.
    void injectTemplates(const std::vector<TemplateBlock>& templates);

    // Parse an HTML string and replace an element's children (innerHTML setter)
    void parseInnerHTML(Element* parent, const std::string& html);

    // Add a shadow-scoped stylesheet to the cascade
    void addShadowStylesheet(ShadowRoot* sr, const std::string& css);

    // Scroll-to-bottom tracking — elements register here instead of walking DOM
    void addScrollToBottomElement(Element* el) { scrollToBottomElements_.insert(el); }
    void removeScrollToBottomElement(Element* el) { scrollToBottomElements_.erase(el); }
    const std::unordered_set<Element*>& scrollToBottomElements() const { return scrollToBottomElements_; }

    // Base path for resolving relative URLs
    void setBasePath(const std::string& path) { basePath_ = path; }
    const std::string& basePath() const { return basePath_; }

    // Viewport for @media evaluation. Call before parse() so stylesheets are
    // filtered against the real viewport; calling again later (window resize)
    // re-evaluates every retained sheet against the new size.
    void setMediaViewport(float w, float h);

    // Color scheme ("light" or "dark") for @media (prefers-color-scheme)
    // evaluation. Same contract as setMediaViewport: call before parse();
    // calling again later (OS theme flip, settings change) re-evaluates every
    // retained sheet and marks the document dirty for restyle.
    void setMediaColorScheme(const std::string& scheme);

    // The current media evaluation context (viewport + color scheme). Default
    // constructed (0x0, light) until setMediaViewport/setMediaColorScheme run.
    const htmlayout::css::MediaContext& mediaContext() const { return mediaContext_; }

    // Bumped whenever the media context actually changes (resize, scheme
    // flip). window.matchMedia re-evaluates its live MediaQueryLists when this
    // moves — a cheap "did anything media-relevant happen" probe per realm.
    uint64_t mediaGeneration() const { return mediaGeneration_; }

    // True between a media-context change and the resolveStyles() that
    // consumes it. matchMedia change delivery waits for this to clear so JS
    // observers always see styles consistent with the new context.
    bool mediaRestylePending() const { return mediaRebuilt_; }

    // ---------- Selection + live Range registry --------------------------
    // The Document owns a single Selection (window.getSelection()) and keeps
    // a set of live Range objects so it can update their endpoints when the
    // DOM mutates. Range::setDocument() registers/unregisters itself.
    Selection* selection();
    void registerRange(Range* r);
    void unregisterRange(Range* r);

    // Notify live ranges about mutations.
    // Call BEFORE performing the removal so ranges can observe parent/index.
    void notifyNodeRemoved(Node* removed);
    // Call AFTER text data is replaced. offset/count is the region that was
    // replaced; newLen is the length of its replacement.
    void notifyTextDataChanged(Node* node, int offset, int count, int newLen);
    // Call AFTER splitText. `tail` is the new node holding [offset, end).
    void notifyTextSplit(Node* node, int offset, Node* tail);
    // Call AFTER a child is inserted at `index` under `parent`.
    void notifyChildInserted(Node* parent, int index);

    // JS runtime hooks: set by the JS engine when DomBindings::install runs.
    using SelectionChangeCallback = void(*)(Document*);
    void setSelectionChangeCallback(SelectionChangeCallback cb) { selectionChangeCb_ = cb; }
    void fireSelectionChange();

    // Fired from freeNode() for every node being queued for destruction (the
    // whole freed subtree, deepest first). Lets the JS layer drop the wrapper's
    // raw Element* and its __bro_elem_map entry the instant the node is doomed —
    // before drainPendingFrees() actually deletes the memory — so no dangling
    // wrapper survives to be dereferenced by a later access or orphan sweep.
    using NodeFreedCallback = void(*)(Document*, Node*);
    void setNodeFreedCallback(NodeFreedCallback cb) { nodeFreedCb_ = cb; }

private:
    // Push this frame's invalidation into the layout tree, right before layout
    // runs. Elements the document could attribute a change to dirty just their
    // own layout node and the ancestor chain above it. An unattributed change
    // (a new stylesheet, a rebuilt tree, a plain Document::markDirty) dirties
    // everything — the whole-document pass bro used to run unconditionally.
    void applyLayoutInvalidation();

    void buildTreeFromGumbo(::GumboNode* node, Element* parentElem);
    void collectElements(Node* node, std::vector<Element*>& out);
    void resolveStylesRecursive(Element* elem, const htmlayout::css::ComputedStyle* parentStyle,
                                bool force = false, bool selectorForce = false,
                                bool hoverForce = false);
    // Add the CSS of any connected <style> element whose rules aren't in the
    // cascade yet (a runtime document.head.appendChild(styleEl)). Incremental —
    // it never clears the cascade, so UA / linked / shadow-scoped sheets and
    // @keyframes/@font-face survive. Runs from resolveStyles when armed.
    // Returns true if a sheet was actually added, which forces a full re-resolve
    // (the new rules can match elements nothing has marked dirty).
    bool reconcileStyleElements();

    // Generated content (::before / ::after) pass. Runs after style resolution.
    // Two modes, chosen by what the stylesheet actually uses:
    //   * counter()/counters()/quotes — a full document-order walk, threading
    //     counter scopes and quote-nesting depth so they resolve correctly.
    //   * anything else — only the elements re-resolved this pass (see
    //     restyled_), because a pseudo-element then depends solely on its
    //     originating element.
    // Populates each element's pseudo content/style via Element::setPseudo.
public:
    struct GenContentState;
private:
    void resolveGeneratedContent();
    void resolveGeneratedContentRecursive(Element* elem, int depth, GenContentState& st);
    void applyPseudo(Element* elem, const char* which, int depth, GenContentState& st);
    static void applyCounterOps(Element* elem, const htmlayout::css::ComputedStyle& style,
                                int depth, GenContentState& st);

    // Elements whose style was re-resolved by the current resolveStyles() pass.
    // Rebuilt every pass; never read outside one, so the pointers can't dangle.
    std::vector<Element*> restyled_;

    // Sticky: some element in this document generates content whose value comes
    // from a counter scope or the quote-nesting depth. Set when such a value is
    // actually resolved (not merely present in a stylesheet — the UA sheet's
    // q::before would make that true everywhere), and never cleared, since the
    // element that tripped it may still be around.
    bool statefulGenContent_ = false;

    template<typename T, typename... Args>
    T* allocateNode(Args&&... args) {
        auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = ptr.get();
        // Every owned node knows its document: NodeHandle needs it to
        // lifetime-check text/comment wrappers, and adoption needs it to
        // retarget a subtree.
        raw->setDocument(this);
        ownedNodes_[raw] = std::move(ptr);
        return raw;
    }

    Node* root_ = nullptr;
    Element* documentElement_ = nullptr;
    Element* body_ = nullptr;
    Element* focusedElement_ = nullptr;
    bool dirty_ = false;
    bool layoutDirty_ = false;
    bool structureDirty_ = false;
    bool rebuildLayoutTree_ = false;
    bool styleElsDirty_ = false;  // a <style> may need adding to the cascade
    bool fullLayout_ = true;
    // The cascade (or an inline write) forced `inherit` on a non-inherited
    // property somewhere, so resolveStylesRecursive cannot scope a restyle by
    // diffing inherited values alone. Sticky once seen. See noteForcedInherit().
    bool forcedInherit_ = false;      // relayout the whole tree, not just what changed
    Perf perf_;
    std::string basePath_;
    std::unordered_map<std::string, Element*> idMap_;
    std::unordered_map<Node*, std::unique_ptr<Node>> ownedNodes_;

    // Nodes moved out of ownedNodes_ by freeNode() but not yet destroyed.
    // Drained by the engine when no other thread is reading the DOM.
    std::vector<std::unique_ptr<Node>> pendingFrees_;
    // Pointer-value index of the nodes currently in pendingFrees_, so liveness
    // of a raw Node* can be answered without dereferencing it. Kept in sync
    // with pendingFrees_ (insert in freeNode, cleared in drainPendingFrees).
    std::unordered_set<Node*> pendingSet_;

    // Persistent layout tree (see layoutRoot()).
    std::unique_ptr<layout::LayoutNodeAdapter> layoutRoot_;

    // CSS cascade
    htmlayout::css::Cascade cascade_;

    // @media evaluation context + retained parsed sheets so a viewport change
    // can re-evaluate every sheet (cascade rebuild) without re-reading sources.
    htmlayout::css::MediaContext mediaContext_;
    bool hasMediaContext_ = false;
    struct RetainedSheet {
        htmlayout::css::Stylesheet sheet;
        void* scope = nullptr;
        htmlayout::css::Origin origin = htmlayout::css::Origin::Author;
    };
    std::vector<RetainedSheet> retainedSheets_;
    // Single funnel for cascade_.addStylesheet — applies the media context
    // and retains the sheet for later re-evaluation.
    void addSheetToCascade(htmlayout::css::Stylesheet sheet, void* scope = nullptr,
                           htmlayout::css::Origin origin = htmlayout::css::Origin::Author);
    // Re-evaluate every retained sheet against mediaContext_ (viewport or
    // color-scheme change) and mark the document dirty.
    void rebuildCascadeForMediaChange();
    // Set by rebuildCascadeForMediaChange; consumed by resolveStyles(), which
    // treats it like a newly-added sheet (full selector-level re-resolve).
    bool mediaRebuilt_ = false;
    // Bumped by setMediaViewport/setMediaColorScheme on an actual change.
    uint64_t mediaGeneration_ = 0;

    // One-shot restyle+relayout after layout when @container rules exist
    // (container sizes are only known post-layout). See performLayout().
    void settleContainerQueries(const std::function<void()>& relayout);
    bool inContainerSettle_ = false;

    // CSS transitions and animations (optional, set by Engine)
    engine::TransitionManager* transitionManager_ = nullptr;
    engine::AnimationManager* animationManager_ = nullptr;
    engine::WebAnimationManager* webAnimationManager_ = nullptr;
    double transitionTime_ = 0;

    // Root element (<html>) resolved font-size in px, captured while resolving
    // the document element's style so rem units in getComputedStyle track the
    // root rather than a hardcoded 16px. Updated each resolveStyles() pass.
    float rootFontSize_ = 16.0f;

    // Elements that need scroll-to-bottom after next layout
    std::unordered_set<Element*> scrollToBottomElements_;

    // Selection + live ranges (registered via Range::setDocument()).
    std::unique_ptr<Selection> selection_;
    std::unordered_set<Range*> liveRanges_;
    SelectionChangeCallback selectionChangeCb_ = nullptr;
    NodeFreedCallback nodeFreedCb_ = nullptr;
};

} // namespace bro::dom
