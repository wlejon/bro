#include "dom/comment_node.h"

namespace bro::dom {

CommentNode::CommentNode(const std::string& data)
    : data_(data)
{
}

void CommentNode::setData(const std::string& text) {
    data_ = text;
}

} // namespace bro::dom
