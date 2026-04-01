#pragma once
#include "dom/node.h"
#include "layout/box.h"
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

    // Layout box for text positioning (set during layout)
    const htmlayout::layout::LayoutBox& layoutBox() const { return layoutBox_; }
    void setLayoutBox(const htmlayout::layout::LayoutBox& box) { layoutBox_ = box; }

private:
    std::string data_;
    htmlayout::layout::LayoutBox layoutBox_;
};

} // namespace bro::dom
