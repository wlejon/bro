#include "dom/document.h"
#include <algorithm>
#include <sstream>

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
    orphans_.push_back(elem);
    return elem;
}

std::shared_ptr<TextNode> Document::createTextNode(const std::string& text) {
    return std::make_shared<TextNode>(text);
}

std::shared_ptr<DocumentFragment> Document::createDocumentFragment() {
    return std::make_shared<DocumentFragment>();
}

void Document::retainOrphan(std::shared_ptr<Element> elem) {
    if (!elem || orphanSet_.count(elem.get())) return;
    orphanSet_.insert(elem.get());
    orphans_.push_back(std::move(elem));
}

void Document::adoptOrphan(Element* elem) {
    if (!orphanSet_.count(elem)) return;
    orphanSet_.erase(elem);
    orphans_.erase(
        std::remove_if(orphans_.begin(), orphans_.end(),
            [elem](const std::shared_ptr<Element>& e) { return e.get() == elem; }),
        orphans_.end());
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

    // Only search dynamic elements if there are orphans (dynamically created elements).
    // This avoids an expensive full-tree walk when all elements came from the parser.
    if (!orphans_.empty() && root_ && root_->nodeType() == NodeType::Element) {
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

} // namespace bro::dom
