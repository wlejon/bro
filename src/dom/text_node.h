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

    // CharacterData methods
    size_t length() const { return data_.size(); }
    std::string substringData(size_t offset, size_t count) const;
    void appendData(const std::string& data);
    void insertData(size_t offset, const std::string& data);
    void deleteData(size_t offset, size_t count);
    void replaceData(size_t offset, size_t count, const std::string& data);

    // Text-specific: splits this node at offset, returns the new tail node.
    // Caller must insert the returned node after this one in the tree.
    TextNode* splitText(size_t offset);

    // Layout box for text positioning (set during layout)
    const htmlayout::layout::LayoutBox& layoutBox() const { return layoutBox_; }
    void setLayoutBox(const htmlayout::layout::LayoutBox& box) { layoutBox_ = box; }

private:
    std::string data_;
    htmlayout::layout::LayoutBox layoutBox_;
};

} // namespace bro::dom
