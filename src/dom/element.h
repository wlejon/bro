#pragma once
#include "dom/node.h"
#include "dom/style_proxy.h"
#include <litehtml.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <cstdint>

namespace bro::dom {

class Document;
class TextNode;

class Element : public Node {
public:
    explicit Element(const std::string& tag);
    ~Element() override { magic_ = 0xDEAD; }

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
    void setAttribute(const std::string& name, const std::string& val);
    void removeAttribute(const std::string& name);

    // Content
    std::string textContent() const;
    void setTextContent(const std::string& text);
    std::string innerHTML() const;
    void setInnerHTML(const std::string& html);

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

    // Selectors
    std::vector<Element*> querySelectorAll(const std::string& selector);
    Element* querySelector(const std::string& selector);
    bool matches(const std::string& selector) const;
    Element* closest(const std::string& selector);

    // Simple selector matching (works without litehtml for dynamic elements)
    bool matchesSimple(const std::string& selector) const;
    void querySelectorAllSimple(const std::string& selector, std::vector<Element*>& out);
    Element* querySelectorSimple(const std::string& selector);

    // Sync inline styles to litehtml element
    void syncStylesToLitehtml();

    // Dirty tracking
    void markDirty();
    bool isDirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

    // Owner document
    void setDocument(Document* doc) { document_ = doc; }
    Document* document() const { return document_; }

    // Litehtml integration
    void setLitehtmlElement(litehtml::element::ptr elem) { litehtml_element_ = std::move(elem); }
    litehtml::element::ptr litehtmlElement() const { return litehtml_element_; }

    // Debug: detect use-after-free
    bool isAlive() const { return magic_ == 0xB00E; }

private:
    std::string tag_;
    std::unordered_map<std::string, std::string> attributes_;
    StyleProxy style_;
    std::unordered_map<std::string, std::vector<uint64_t>> listeners_;
    litehtml::element::ptr litehtml_element_;
    Document* document_ = nullptr;
    bool dirty_ = false;
    uint32_t magic_ = 0xB00E;
};

} // namespace bro::dom
