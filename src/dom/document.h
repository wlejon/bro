#pragma once
#include "dom/node.h"
#include "dom/element.h"
#include "dom/text_node.h"
#include "dom/comment_node.h"
#include "dom/document_fragment.h"
#include <litehtml.h>
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>

namespace bro::dom {

class Document {
public:
    Document();
    ~Document();

    // Parsing
    void parse(const std::string& html, litehtml::document_container* container);
    void buildFrom(litehtml::document::ptr doc);
    void reparse(litehtml::document_container* container);

    // Node creation — Document owns all nodes via ownedNodes_.
    Element* createElement(const std::string& tag);
    TextNode* createTextNode(const std::string& text);
    CommentNode* createComment(const std::string& data);
    DocumentFragment* createDocumentFragment();

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

    // Litehtml integration
    litehtml::document::ptr litehtmlDocument() const { return litehtml_doc_; }

    // ID map management (called by elements when id attribute changes)
    /// Pre-process HTML: extract <template> blocks that litehtml/gumbo would
    /// discard, replacing them with hidden placeholder divs. After buildFrom(),
    /// call injectTemplates() to restore them in the bro::dom tree.
    struct TemplateBlock {
        std::string id;         // original id attribute (or generated)
        std::string attrs;      // other attributes on the <template> tag
        std::string innerHTML;  // raw inner HTML content
    };
    static std::string extractTemplates(const std::string& html,
                                        std::vector<TemplateBlock>& out);

    /// After buildFrom(), call this to populate placeholder elements with
    /// their template content (stored as children, hidden from rendering).
    void injectTemplates(const std::vector<TemplateBlock>& templates);

    /// Check if a litehtml element's white-space CSS preserves newlines
    /// (pre, pre-wrap, pre-line). If so, text with \n should be converted
    /// to HTML with <br> when syncing to litehtml.
    static bool preservesNewlines(const litehtml::element::ptr& lhElem);

    /// Convert a text string to HTML suitable for litehtml, replacing \n
    /// with <br> if the parent element preserves newlines. Escapes HTML entities.
    /// Returns empty string if text is empty.
    static std::string textToLitehtmlHtml(const std::string& text,
                                          const litehtml::element::ptr& parentLh);

    void registerElementId(const std::string& id, Element* elem);
    void unregisterElementId(const std::string& id);

    // Find our Element wrapper for a litehtml element pointer
    Element* findElementByLitehtml(const litehtml::element::ptr& lhElem);

    // Sync dynamic DOM mutations to litehtml tree for rendering
    void syncAppendToLitehtml(Element* child, Element* parent);
    void syncInsertBeforeLitehtml(Element* newChild, Element* refChild, Element* parent);
    void syncRemoveFromLitehtml(Element* child, Element* parent);

    // Parse an HTML string and replace an element's children (innerHTML setter)
    void parseInnerHTML(Element* parent, const std::string& html);

private:
    void buildTreeFromLitehtml(litehtml::element::ptr root, Element* parentElem);
    void collectElements(Node* node, std::vector<Element*>& out);
    void linkElementToLitehtml(litehtml::element::ptr lh, Element* elem);
    void unlinkLitehtmlRecursive(Element* elem);

    // Helper: allocate a node owned by this document, returns raw pointer.
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
    litehtml::document::ptr litehtml_doc_;
    std::unordered_map<std::string, Element*> idMap_;

    // Document owns ALL nodes. This is the sole owner of every Node in the tree.
    std::vector<std::unique_ptr<Node>> ownedNodes_;

    // Fast lookup: litehtml element raw pointer -> our Element wrapper.
    struct LitehtmlPtrHash {
        size_t operator()(const litehtml::element::ptr& p) const {
            return std::hash<void*>{}(p.get());
        }
    };
    struct LitehtmlPtrEqual {
        bool operator()(const litehtml::element::ptr& a, const litehtml::element::ptr& b) const {
            return a.get() == b.get();
        }
    };
    std::unordered_map<litehtml::element::ptr, Element*, LitehtmlPtrHash, LitehtmlPtrEqual> litehtmlMap_;
};

} // namespace bro::dom
