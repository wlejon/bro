// Element text and markup: `textContent`, the writes that reparse or replace
// a subtree, and innerHTML / outerHTML over the serializers (serialize.cpp).
//
// Split out of element.cpp, which had grown past the size this repo keeps its
// translation units to. Nothing here touches layout, style resolution or the
// event machinery; it is the string half of an Element and reads as one.

#include "dom/element.h"
#include "dom/comment_node.h"
#include "dom/document.h"
#include "dom/serialize.h"
#include "dom/text_node.h"

#include <cctype>
#include <sstream>
#include <string>
#include <unordered_map>

namespace bro::dom {

std::string Element::textContent() const {
    std::string result;
    for (const auto& child : children_) {
        if (child->nodeType() == NodeType::Text) {
            auto* text = static_cast<const TextNode*>(child);
            result += text->data();
        } else if (child->nodeType() == NodeType::Element) {
            auto* elem = static_cast<const Element*>(child);
            result += elem->textContent();
        }
    }
    return result;
}

void Element::setTextContent(const std::string& text) {
    // A document with MutationObservers skips the in-place rewrite: the DOM
    // replaces the text node, and an observer is owed that childList record,
    // not a characterData one.
    if (children_.size() == 1 && children_[0]->nodeType() == NodeType::Text &&
        !(document_ && document_->hasMutationObservers())) {
        auto* existing = static_cast<TextNode*>(children_[0]);
        if (existing->data() == text) return;
        // Rewriting the lone text child in place leaves the tree shape alone, so
        // the layout tree stays valid (its adapter reads the TextNode live) and
        // this is a plain layout invalidation rather than a structural rebuild —
        // the difference between relaying out one element and relaying out the
        // document. Emptying the element still takes the slow path: layout drops
        // empty text nodes, so the tree really does change shape.
        if (!text.empty()) {
            existing->setData(text);
            markDirty();
            return;
        }
    }

    // Clear old children. Element children are DETACHED rather than destroyed
    // (Document::releaseChildrenPreservingElements says why); text and comment
    // nodes go.
    if (document_) {
        document_->replaceAllChildren(this, [&] {
            if (!text.empty()) appendChild(document_->createTextNode(text));
        });
    } else {
        for (auto* child : children_) child->setParent(nullptr);
        children_.clear();
    }

    markDirty();
    markStructureDirty();
}

std::string Element::innerHTML() const { return serializeHtmlChildren(this); }

std::string Element::outerHTML() const { return serializeHtmlNode(this); }

void Element::setInnerHTML(const std::string& html) {
    // Setting a <template>'s innerHTML replaces its CONTENT's children: the
    // markup is parsed into the inert fragment, never into the template's own
    // child list, so it neither renders nor answers a document query.
    if (document_ && (tag_ == "TEMPLATE") && !isTemplateContent_) {
        if (!templateContent_) {
            setTemplateContent(document_->createElement("#DOCUMENT-FRAGMENT"));
        }
        Element* content = templateContent_;
        document_->replaceAllChildren(content, [&] { document_->parseInnerHTML(content, html); });
        return;
    }
    if (document_) {
        document_->replaceAllChildren(this, [&] { document_->parseInnerHTML(this, html); });
        return;
    }
    auto oldKids = children_;
    for (auto& child : oldKids) {
        child->setParent(nullptr);
    }
    children_.clear();
    markDirty();
}

void Element::setOuterHTML(const std::string& html) {
    if (!parent_ || !document_) return;

    // Parse the new HTML into a temporary container
    auto* tempContainer = document_->createElement("DIV");
    document_->parseInnerHTML(tempContainer, html);

    // Insert all parsed children before this element in the parent
    auto newChildren = tempContainer->childNodes();
    for (auto* child : newChildren) {
        child->setParent(nullptr);
    }
    tempContainer->childNodes().clear();

    for (auto* child : newChildren) {
        parent_->insertBefore(child, this);
        if (child->nodeType() == NodeType::Element) {
            auto* childElem = static_cast<Element*>(child);
            childElem->setDocument(document_);
        }
    }

    // Unregister this element's ID before removal
    if (!id().empty()) {
        document_->unregisterElementId(id(), this);
    }

    // Remove this element from parent. removeChild marks the PARENT — its child
    // list is the one that changed, and a mark left on this element would never
    // be read since it is leaving the tree.
    parent_->removeChild(this);

    // Free the temporary container
    document_->freeNode(tempContainer);
}

} // namespace bro::dom
