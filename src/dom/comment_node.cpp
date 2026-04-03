#include "dom/comment_node.h"
#include <algorithm>

namespace bro::dom {

CommentNode::CommentNode(const std::string& data)
    : data_(data)
{
}

void CommentNode::setData(const std::string& text) {
    data_ = text;
}

std::string CommentNode::substringData(size_t offset, size_t count) const {
    if (offset > data_.size()) return {};
    return data_.substr(offset, count);
}

void CommentNode::appendData(const std::string& data) {
    data_ += data;
}

void CommentNode::insertData(size_t offset, const std::string& data) {
    if (offset > data_.size()) offset = data_.size();
    data_.insert(offset, data);
}

void CommentNode::deleteData(size_t offset, size_t count) {
    if (offset > data_.size()) return;
    data_.erase(offset, count);
}

void CommentNode::replaceData(size_t offset, size_t count, const std::string& data) {
    if (offset > data_.size()) return;
    size_t eraseCount = std::min(count, data_.size() - offset);
    data_.erase(offset, eraseCount);
    data_.insert(offset, data);
}

} // namespace bro::dom
