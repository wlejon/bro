#include "dom/document.h"
#include <litehtml/html_tag.h>
#include <algorithm>
#include <sstream>

// Helper: get mutable children list from a litehtml element.
// element::children() is const; html_tag::children() is non-const.
static std::list<litehtml::element::ptr>& mutableChildren(litehtml::element::ptr& el) {
    return static_cast<litehtml::html_tag*>(el.get())->children();
}

namespace bro::dom {

Document::Document() = default;
Document::~Document() = default;

void Document::parse(const std::string& html, litehtml::document_container* container) {
    // Parse with litehtml
    litehtml_doc_ = litehtml::document::createFromString(html, container);
    if (!litehtml_doc_) return;

    // Clear any existing tree
    root_.reset();
    documentElement_ = nullptr;
    body_ = nullptr;
    idMap_.clear();
    litehtmlMap_.clear();

    // Build our Element tree from the litehtml tree
    auto lh_root = litehtml_doc_->root();
    if (!lh_root) return;

    const char* rootTag = lh_root->get_tagName();
    auto rootElem = std::make_shared<Element>(rootTag ? rootTag : "html");
    rootElem->setLitehtmlElement(lh_root);
    rootElem->setDocument(this);
    litehtmlMap_[lh_root] = rootElem.get();
    root_ = rootElem;
    documentElement_ = rootElem.get();

    // Copy attributes from litehtml root
    // Build children recursively
    buildTreeFromLitehtml(lh_root, rootElem.get());

    // Find <body> element
    for (auto* child : rootElem->children()) {
        if (child->tagName() == "BODY") {
            body_ = child;
            break;
        }
    }

    // Register IDs
    std::vector<Element*> allElems;
    collectElements(root_.get(), allElems);
    for (auto* elem : allElems) {
        std::string elemId = elem->id();
        if (!elemId.empty()) {
            idMap_[elemId] = elem;
        }
    }

    dirty_ = false;
}

void Document::buildFrom(litehtml::document::ptr doc) {
    litehtml_doc_ = doc;
    if (!litehtml_doc_) return;

    root_.reset();
    documentElement_ = nullptr;
    body_ = nullptr;
    idMap_.clear();
    litehtmlMap_.clear();

    auto lh_root = litehtml_doc_->root();
    if (!lh_root) return;

    const char* rootTag = lh_root->get_tagName();
    auto rootElem = std::make_shared<Element>(rootTag ? rootTag : "html");
    rootElem->setLitehtmlElement(lh_root);
    rootElem->setDocument(this);
    litehtmlMap_[lh_root] = rootElem.get();
    root_ = rootElem;
    documentElement_ = rootElem.get();

    buildTreeFromLitehtml(lh_root, rootElem.get());

    for (auto* child : rootElem->children()) {
        if (child->tagName() == "BODY") {
            body_ = child;
            break;
        }
    }

    std::vector<Element*> allElems;
    collectElements(root_.get(), allElems);
    for (auto* elem : allElems) {
        std::string elemId = elem->id();
        if (!elemId.empty()) {
            idMap_[elemId] = elem;
        }
    }

    dirty_ = false;
}

void Document::reparse(litehtml::document_container* container) {
    if (!documentElement_) return;

    // Re-serialize the tree to HTML
    std::string html = documentElement_->innerHTML();

    // Re-parse
    parse(html, container);
}

std::shared_ptr<Element> Document::createElement(const std::string& tag) {
    auto elem = std::make_shared<Element>(tag);
    elem->setDocument(this);
    holdNewElement(elem);
    return elem;
}

void Document::holdNewElement(std::shared_ptr<Element> elem) {
    newElements_[elem.get()] = std::move(elem);
}

void Document::releaseNewElement(Element* elem) {
    newElements_.erase(elem);
}

std::shared_ptr<TextNode> Document::createTextNode(const std::string& text) {
    return std::make_shared<TextNode>(text);
}

std::shared_ptr<DocumentFragment> Document::createDocumentFragment() {
    return std::make_shared<DocumentFragment>();
}

Element* Document::getElementById(const std::string& id) {
    auto it = idMap_.find(id);
    if (it != idMap_.end()) {
        return it->second;
    }
    return nullptr;
}

Element* Document::querySelector(const std::string& selector) {
    // Try litehtml first
    if (litehtml_doc_ && litehtml_doc_->root()) {
        auto found = litehtml_doc_->root()->select_one(selector);
        if (found) {
            auto it = litehtmlMap_.find(found);
            if (it != litehtmlMap_.end()) return it->second;
        }
    }
    // Fallback: simple selector matching on bro::dom tree (for dynamic elements)
    if (root_ && root_->nodeType() == NodeType::Element) {
        return static_cast<Element*>(root_.get())->querySelectorSimple(selector);
    }
    return nullptr;
}

std::vector<Element*> Document::querySelectorAll(const std::string& selector) {
    std::vector<Element*> result;

    // litehtml results (fast, covers parsed HTML elements)
    if (litehtml_doc_ && litehtml_doc_->root()) {
        auto found = litehtml_doc_->root()->select_all(selector);
        for (auto& lh_elem : found) {
            auto it = litehtmlMap_.find(lh_elem);
            if (it != litehtmlMap_.end()) {
                result.push_back(it->second);
            }
        }
    }

    // Also search the bro::dom tree for dynamically-created elements
    // that may not be in litehtml's selector index.
    if (root_ && root_->nodeType() == NodeType::Element) {
        size_t before = result.size();
        static_cast<Element*>(root_.get())->querySelectorAllSimple(selector, result);
        // Deduplicate only if we added new results
        if (result.size() > before) {
            std::sort(result.begin(), result.end());
            result.erase(std::unique(result.begin(), result.end()), result.end());
        }
    }

    return result;
}

std::string Document::title() const {
    if (!documentElement_) return {};

    // Walk the tree looking for a <TITLE> element
    std::vector<Element*> allElems;
    const_cast<Document*>(this)->collectElements(root_.get(), allElems);
    for (auto* elem : allElems) {
        if (elem->tagName() == "TITLE") {
            return elem->textContent();
        }
    }
    return {};
}

void Document::setTitle(const std::string& title) {
    if (!documentElement_) return;

    std::vector<Element*> allElems;
    collectElements(root_.get(), allElems);
    for (auto* elem : allElems) {
        if (elem->tagName() == "TITLE") {
            elem->setTextContent(title);
            return;
        }
    }

    // If no <title> found, try to find <head> and add one
    for (auto* elem : allElems) {
        if (elem->tagName() == "HEAD") {
            auto titleElem = createElement("title");
            titleElem->setTextContent(title);
            elem->appendChild(std::move(titleElem));
            return;
        }
    }
}

void Document::registerElementId(const std::string& id, Element* elem) {
    if (!id.empty() && elem) {
        idMap_[id] = elem;
    }
}

void Document::unregisterElementId(const std::string& id) {
    idMap_.erase(id);
}

void Document::buildTreeFromLitehtml(litehtml::element::ptr root, Element* parentElem) {
    if (!root) return;

    for (auto& lh_child : root->children()) {
        std::string tag = lh_child->get_tagName() ? lh_child->get_tagName() : "";

        if (tag.empty()) {
            // Text node - litehtml represents text as elements with empty tag
            litehtml::string text;
            lh_child->get_text(text);
            auto textNode = std::make_shared<TextNode>(text);
            parentElem->appendChild(std::move(textNode));
        } else {
            auto childElem = std::make_shared<Element>(tag);
            childElem->setLitehtmlElement(lh_child);
            childElem->setDocument(this);
            litehtmlMap_[lh_child] = childElem.get();

            // Copy known attributes
            static const char* commonAttrs[] = {
                "id", "class", "style", "href", "src", "alt", "title",
                "name", "value", "type", "placeholder",
                "data-action", "data-setting", "data-control",
                "width", "height", "disabled", "checked", "selected",
                nullptr
            };
            for (int a = 0; commonAttrs[a] != nullptr; ++a) {
                const char* val = lh_child->get_attr(commonAttrs[a]);
                if (val) {
                    childElem->setAttribute(commonAttrs[a], val);
                }
            }

            Element* rawPtr = childElem.get();
            parentElem->appendChild(std::move(childElem));

            // Recurse
            buildTreeFromLitehtml(lh_child, rawPtr);
        }
    }
}

void Document::collectElements(Node* node, std::vector<Element*>& out) {
    if (!node) return;
    if (node->nodeType() == NodeType::Element) {
        out.push_back(static_cast<Element*>(node));
    }
    for (auto& child : node->childNodes()) {
        collectElements(child.get(), out);
    }
}

Element* Document::findElementByLitehtml(const litehtml::element::ptr& lhElem) {
    if (!lhElem) return nullptr;
    auto it = litehtmlMap_.find(lhElem);
    return (it != litehtmlMap_.end()) ? it->second : nullptr;
}

// ---------------------------------------------------------------------------
// Dynamic DOM → litehtml sync
// ---------------------------------------------------------------------------

void Document::syncAppendToLitehtml(Element* child, Element* parent) {
    if (!child || !parent || !litehtml_doc_) return;
    auto parentLh = parent->litehtmlElement();
    if (!parentLh) return;
    if (child->litehtmlElement()) return; // already linked

    std::string html = child->outerHTML();
    size_t beforeCount = parentLh->children().size();

    // Add to litehtml element tree (CSS is applied, render items are NOT
    // reliably created here — the render tree rebuild handles that).
    litehtml_doc_->append_children_from_string(*parentLh, html.c_str(), false);

    // Link the newly created litehtml element to our bro::dom element
    auto& lhChildren = parentLh->children();
    if (lhChildren.size() > beforeCount) {
        auto it = lhChildren.begin();
        std::advance(it, beforeCount);
        linkElementToLitehtml(*it, child);
    }

    markStructureDirty();
}

void Document::syncInsertBeforeLitehtml(Element* newChild, Element* refChild,
                                         Element* parent) {
    if (!newChild || !parent || !litehtml_doc_) return;
    auto parentLh = parent->litehtmlElement();
    if (!parentLh) return;
    if (newChild->litehtmlElement()) return;

    std::string html = newChild->outerHTML();
    size_t beforeCount = parentLh->children().size();

    litehtml_doc_->append_children_from_string(*parentLh, html.c_str(), false);

    auto& lhChildren = parentLh->children();
    if (lhChildren.size() <= beforeCount) return;

    // Link the new element (currently at end)
    auto newLh = lhChildren.back();
    linkElementToLitehtml(newLh, newChild);

    // Move to before the reference element in the litehtml children list
    if (refChild && refChild->litehtmlElement()) {
        auto refLh = refChild->litehtmlElement();
        auto& mutChildren = mutableChildren(parentLh);
        auto newIt = std::prev(mutChildren.end());
        auto refIt = std::find(mutChildren.begin(), mutChildren.end(), refLh);
        if (refIt != mutChildren.end() && newIt != refIt) {
            mutChildren.splice(refIt, mutChildren, newIt);
        }
    }

    markStructureDirty();
}

void Document::syncRemoveFromLitehtml(Element* child, Element* parent) {
    if (!child || !parent) return;
    auto childLh = child->litehtmlElement();
    auto parentLh = parent->litehtmlElement();
    if (!childLh || !parentLh) return;

    // Remove from litehtml element tree
    parentLh->removeChild(childLh);

    // Clean up litehtmlMap entries for the whole subtree
    unlinkLitehtmlRecursive(child);

    markStructureDirty();
}

void Document::linkElementToLitehtml(litehtml::element::ptr lh, Element* elem) {
    if (!lh || !elem) return;
    elem->setLitehtmlElement(lh);
    litehtmlMap_[lh] = elem;

    // Register ID if present
    std::string elemId = elem->id();
    if (!elemId.empty()) {
        idMap_[elemId] = elem;
    }

    // Recursively link child elements (skip litehtml text nodes)
    auto& lhChildren = lh->children();
    auto broChildren = elem->children(); // Element children only

    auto lhIt = lhChildren.begin();
    size_t broIdx = 0;

    while (lhIt != lhChildren.end() && broIdx < broChildren.size()) {
        const char* tag = (*lhIt)->get_tagName();
        if (tag && tag[0] != '\0') {
            // Element node — link to corresponding bro::dom child
            linkElementToLitehtml(*lhIt, broChildren[broIdx]);
            broIdx++;
        }
        ++lhIt;
    }
}

void Document::unlinkLitehtmlRecursive(Element* elem) {
    if (!elem) return;
    auto lh = elem->litehtmlElement();
    if (lh) {
        litehtmlMap_.erase(lh);
        elem->setLitehtmlElement(nullptr);
    }
    for (auto* child : elem->children()) {
        unlinkLitehtmlRecursive(child);
    }
}

} // namespace bro::dom
