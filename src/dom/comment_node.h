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

    // CharacterData methods
    size_t length() const { return data_.size(); }
    std::string substringData(size_t offset, size_t count) const;
    void appendData(const std::string& data);
    void insertData(size_t offset, const std::string& data);
    void deleteData(size_t offset, size_t count);
    void replaceData(size_t offset, size_t count, const std::string& data);

private:
    std::string data_;
};

} // namespace bro::dom
