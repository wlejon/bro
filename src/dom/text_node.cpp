#include "dom/text_node.h"

namespace bro::dom {

TextNode::TextNode(const std::string& data)
    : data_(data)
{
}

void TextNode::setData(const std::string& text) {
    data_ = text;
}

} // namespace bro::dom
