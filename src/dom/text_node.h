#pragma once
#include "dom/node.h"
#include <string>

namespace bro::dom {

class TextNode : public Node {
public:
    explicit TextNode(const std::string& data = "");
    ~TextNode() override = default;

    NodeType nodeType() const override { return NodeType::Text; }
    std::string nodeName() const override { return "#text"; }

    const std::string& data() const { return data_; }
    void setData(const std::string& text);

private:
    std::string data_;
};

} // namespace bro::dom
