#include "dom/text_node.h"
#include <algorithm>

namespace bro::dom {

TextNode::TextNode(const std::string& data)
    : data_(data)
{
}

void TextNode::setData(const std::string& text) {
    data_ = text;
}

std::string TextNode::substringData(size_t offset, size_t count) const {
    if (offset > data_.size()) return {};
    return data_.substr(offset, count);
}

void TextNode::appendData(const std::string& data) {
    data_ += data;
}

void TextNode::insertData(size_t offset, const std::string& data) {
    if (offset > data_.size()) offset = data_.size();
    data_.insert(offset, data);
}

void TextNode::deleteData(size_t offset, size_t count) {
    if (offset > data_.size()) return;
    data_.erase(offset, count);
}

void TextNode::replaceData(size_t offset, size_t count, const std::string& data) {
    if (offset > data_.size()) return;
    size_t eraseCount = std::min(count, data_.size() - offset);
    data_.erase(offset, eraseCount);
    data_.insert(offset, data);
}

TextNode* TextNode::splitText(size_t offset) {
    if (offset > data_.size()) offset = data_.size();
    std::string tail = data_.substr(offset);
    data_ = data_.substr(0, offset);
    auto* newNode = new TextNode(tail);
    // Caller is responsible for inserting into tree and taking ownership
    return newNode;
}

} // namespace bro::dom
