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

namespace bro::layout { class FontManager; }
namespace bro::render { class Renderer; }

namespace bro::dom {

class Document {
public:
    Document();
    ~Document();

    // Parsing — parse HTML with gumbo, extract <style> CSS, build DOM tree
    void parse(const std::string& html, const std::string& userCss = {});

    // Node creation — Document owns all nodes via ownedNodes_.
    Element* createElement(const std::string& tag);
    TextNode* createTextNode(const std::string& text);
    CommentNode* createComment(const std::string& data);
    DocumentFragment* createDocumentFragment();

    // Allocate a ShadowRoot owned by this document
    ShadowRoot* allocateShadowRoot(Element* host, ShadowRoot::Mode mode);

    // Free a node from ownedNodes_ (called after removal from tree + JS wrapper invalidation)
    void freeNode(Node* node);

    // Queries
    Element* getElementById(const std::string& id);
    Element* querySelector(const std::string& selector);
    std::vector<Element*> querySelectorAll(const std::string& selector);

    // Tree accessors
    Element* body() const { return body_; }
    Element* documentElement() const { return documentElement_; }

    // Focus tracking
    Element* activeElement() const { return focusedElement_ ? focusedElement_ : body_; }
    void setActiveElement(Element* el) { focusedElement_ = el; }

    // Title
    std::string title() const;
    void setTitle(const std::string& title);

    // Dirty tracking
    void markDirty() { dirty_ = true; }
    bool isDirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

    // Structure dirty — DOM nodes added/removed, render tree needs rebuild
    void markStructureDirty() { structureDirty_ = true; dirty_ = true; }
    bool isStructureDirty() const { return structureDirty_; }
    void clearStructureDirty() { structureDirty_ = false; }

    // CSS cascade — add stylesheets, resolve styles
    htmlayout::css::Cascade& cascade() { return cascade_; }

    // Resolve computed styles for all elements in the tree
    void resolveStyles();

    // Perform layout on the tree using htmlayout
    void performLayout(float viewportWidth, htmlayout::layout::TextMetrics& metrics);
    void performLayout(float viewportWidth, float viewportHeight, htmlayout::layout::TextMetrics& metrics);

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

private:
    void buildTreeFromGumbo(::GumboNode* node, Element* parentElem);
    void collectElements(Node* node, std::vector<Element*>& out);
    void resolveStylesRecursive(Element* elem, const htmlayout::css::ComputedStyle* parentStyle);

    template<typename T, typename... Args>
    T* allocateNode(Args&&... args) {
        auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = ptr.get();
        ownedNodes_.push_back(std::move(ptr));
        return raw;
    }

    Node* root_ = nullptr;
    Element* documentElement_ = nullptr;
    Element* body_ = nullptr;
    Element* focusedElement_ = nullptr;
    bool dirty_ = false;
    bool structureDirty_ = false;
    std::string basePath_;
    std::unordered_map<std::string, Element*> idMap_;
    std::vector<std::unique_ptr<Node>> ownedNodes_;

    // CSS cascade
    htmlayout::css::Cascade cascade_;

    // Elements that need scroll-to-bottom after next layout
    std::unordered_set<Element*> scrollToBottomElements_;
};

} // namespace bro::dom
