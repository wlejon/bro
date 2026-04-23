#include "dom/text_node.h"
#include "dom/document.h"
#include "dom/element.h"
#include <algorithm>

namespace bro::dom {

// Walk up to find the owning Document so mutations can update live ranges.
static Document* ownerDoc(Node* node) {
    for (Node* n = node; n; n = n->parentNode()) {
        if (n->nodeType() == NodeType::Element)
            return static_cast<Element*>(n)->document();
    }
    return nullptr;
}

TextNode::TextNode(const std::string& data)
    : data_(data)
{
}

void TextNode::setData(const std::string& text) {
    int oldLen = static_cast<int>(data_.size());
    int newLen = static_cast<int>(text.size());
    data_ = text;
    if (auto* doc = ownerDoc(this))
        doc->notifyTextDataChanged(this, 0, oldLen, newLen);
}

std::string TextNode::substringData(size_t offset, size_t count) const {
    if (offset > data_.size()) return {};
    return data_.substr(offset, count);
}

void TextNode::appendData(const std::string& data) {
    int before = static_cast<int>(data_.size());
    data_ += data;
    if (auto* doc = ownerDoc(this))
        doc->notifyTextDataChanged(this, before, 0, static_cast<int>(data.size()));
}

void TextNode::insertData(size_t offset, const std::string& data) {
    if (offset > data_.size()) offset = data_.size();
    data_.insert(offset, data);
    if (auto* doc = ownerDoc(this))
        doc->notifyTextDataChanged(this, static_cast<int>(offset), 0,
                                   static_cast<int>(data.size()));
}

void TextNode::deleteData(size_t offset, size_t count) {
    if (offset > data_.size()) return;
    size_t actual = std::min(count, data_.size() - offset);
    data_.erase(offset, actual);
    if (auto* doc = ownerDoc(this))
        doc->notifyTextDataChanged(this, static_cast<int>(offset),
                                   static_cast<int>(actual), 0);
}

void TextNode::replaceData(size_t offset, size_t count, const std::string& data) {
    if (offset > data_.size()) return;
    size_t eraseCount = std::min(count, data_.size() - offset);
    data_.erase(offset, eraseCount);
    data_.insert(offset, data);
    if (auto* doc = ownerDoc(this))
        doc->notifyTextDataChanged(this, static_cast<int>(offset),
                                   static_cast<int>(eraseCount),
                                   static_cast<int>(data.size()));
}

TextNode* TextNode::splitText(size_t offset) {
    if (offset > data_.size()) offset = data_.size();
    std::string tail = data_.substr(offset);
    data_ = data_.substr(0, offset);
    auto* newNode = new TextNode(tail);
    if (auto* doc = ownerDoc(this))
        doc->notifyTextSplit(this, static_cast<int>(offset), newNode);
    // Caller is responsible for inserting into tree and taking ownership
    return newNode;
}

} // namespace bro::dom
