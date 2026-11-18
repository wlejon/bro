#pragma once
#include "dom/node.h"
#include <string>

namespace bro::dom {

// A document type node (DOM §4.6): `<!DOCTYPE html>`, or one made by
// DOMImplementation.createDocumentType. A document holds at most one, before
// its document element (Document::doctype); like the document element it has
// no parentNode in this tree, and the host answers the document as its parent.
class DocumentType : public Node {
public:
    DocumentType(std::string name, std::string publicId, std::string systemId)
        : name_(std::move(name)), publicId_(std::move(publicId)), systemId_(std::move(systemId)) {}
    ~DocumentType() override = default;

    NodeType nodeType() const override { return NodeType::DocumentType; }
    std::string nodeName() const override { return name_; }

    const std::string& name() const { return name_; }
    const std::string& publicId() const { return publicId_; }
    const std::string& systemId() const { return systemId_; }

private:
    std::string name_;
    std::string publicId_;
    std::string systemId_;
};

} // namespace bro::dom
