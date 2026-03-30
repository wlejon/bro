#pragma once
#include "dom/node.h"
#include <string>

namespace bro::dom {

class CommentNode : public Node {
public:
    explicit CommentNode(const std::string& data = "");
    ~CommentNode() override = default;

    NodeType nodeType() const override { return NodeType::Comment; }
    std::string nodeName() const override { return "#comment"; }

    const std::string& data() const { return data_; }
    void setData(const std::string& text);

private:
    std::string data_;
};

} // namespace bro::dom
