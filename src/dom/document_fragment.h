#pragma once
#include "dom/node.h"
#include <string>

namespace bro::dom {

class DocumentFragment : public Node {
public:
    DocumentFragment() = default;
    ~DocumentFragment() override = default;

    NodeType nodeType() const override { return NodeType::DocumentFragment; }
    std::string nodeName() const override { return "#document-fragment"; }
};

} // namespace bro::dom
