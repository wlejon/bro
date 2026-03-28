#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace bro::dom {

enum class NodeType : uint8_t {
    Element = 1,
    Text = 3,
    Document = 9
};

class Element;  // forward

class Node {
public:
    virtual ~Node() = default;
    virtual NodeType nodeType() const = 0;
    virtual std::string nodeName() const = 0;

    Node* parentNode() const { return parent_; }
    void setParent(Node* p) { parent_ = p; }

    std::vector<std::shared_ptr<Node>>& childNodes() { return children_; }
    const std::vector<std::shared_ptr<Node>>& childNodes() const { return children_; }

    void appendChild(std::shared_ptr<Node> child);
    void removeChild(Node* child);
    void insertBefore(std::shared_ptr<Node> newChild, Node* refChild);

    uint32_t nodeId() const { return id_; }

protected:
    Node* parent_ = nullptr;
    std::vector<std::shared_ptr<Node>> children_;
    uint32_t id_ = nextId();
    static uint32_t nextId() { static uint32_t counter = 0; return ++counter; }
};

} // namespace bro::dom
