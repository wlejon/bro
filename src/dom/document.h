#pragma once
#include "dom/node.h"
#include "dom/element.h"
#include "dom/text_node.h"
#include <litehtml.h>
#include <string>
#include <memory>
#include <unordered_map>
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

    // Node creation
    std::shared_ptr<Element> createElement(const std::string& tag);
    std::shared_ptr<TextNode> createTextNode(const std::string& text);

    // Queries
    Element* getElementById(const std::string& id);
    Element* querySelector(const std::string& selector);
    std::vector<Element*> querySelectorAll(const std::string& selector);

    // Tree accessors
    Element* body() const { return body_; }
    Element* documentElement() const { return documentElement_; }

    // Title
    std::string title() const;
    void setTitle(const std::string& title);

    // Dirty tracking
    void markDirty() { dirty_ = true; }
    bool isDirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

    // Litehtml integration
    litehtml::document::ptr litehtmlDocument() const { return litehtml_doc_; }

    // ID map management (called by elements when id attribute changes)
    void registerElementId(const std::string& id, Element* elem);
    void unregisterElementId(const std::string& id);

    // Transfer ownership of an orphan element (created via createElement) to its new parent.
    void adoptOrphan(Element* elem);

    // Access orphan list (for shared_ptr lookup during appendChild).
    const std::vector<std::shared_ptr<Element>>& orphans() const { return orphans_; }

    // Find our Element wrapper for a litehtml element pointer
    Element* findElementByLitehtml(const litehtml::element::ptr& lhElem);

private:
    void buildTreeFromLitehtml(litehtml::element::ptr root, Element* parentElem);
    void collectElements(Node* node, std::vector<Element*>& out);

    std::shared_ptr<Node> root_;
    Element* documentElement_ = nullptr;
    Element* body_ = nullptr;
    bool dirty_ = false;
    litehtml::document::ptr litehtml_doc_;
    std::unordered_map<std::string, Element*> idMap_;

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

    // Orphan elements created via createElement that haven't been appended yet.
    // Prevents use-after-free when JS holds a reference to a created element.
    std::vector<std::shared_ptr<Element>> orphans_;
};

} // namespace bro::dom
