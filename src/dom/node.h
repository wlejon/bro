#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace bro::dom {

enum class NodeType : uint8_t {
    Element = 1,
    Text = 3,
    Comment = 8,
    Document = 9,
    DocumentFragment = 11
};

class Element;   // forward
class Document;  // forward

class Node {
public:
    virtual ~Node() = default;
    virtual NodeType nodeType() const = 0;
    virtual std::string nodeName() const = 0;

    // Owner document. Every node allocated through Document::allocateNode gets
    // this set, so text and comment nodes can be lifetime-checked (NodeHandle)
    // and adopted across documents exactly like elements can.
    void setDocument(Document* doc) { document_ = doc; }
    Document* document() const { return document_; }

    Node* parentNode() const { return parent_; }
    void setParent(Node* p) { parent_ = p; }

    std::vector<Node*>& childNodes() { return children_; }
    const std::vector<Node*>& childNodes() const { return children_; }

    void appendChild(Node* child);
    void removeChild(Node* child);
    void insertBefore(Node* newChild, Node* refChild);

    uint32_t nodeId() const { return id_; }

protected:
    Node* parent_ = nullptr;
    Document* document_ = nullptr;
    std::vector<Node*> children_;
    uint32_t id_ = nextId();
    static uint32_t nextId() { static uint32_t counter = 0; return ++counter; }
};

} // namespace bro::dom
