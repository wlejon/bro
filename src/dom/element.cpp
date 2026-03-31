#include "dom/element.h"
#include "dom/document.h"
#include "dom/shadow_root.h"
#include "util/log.h"
#include "dom/text_node.h"
#include "dom/comment_node.h"
#include "layout/bro_el_text.h"
#include <litehtml.h>
#include <litehtml/el_text.h>
#include <litehtml/render_item.h>

using bro_el_text = bro::layout::BroElText;
#include <algorithm>
#include <sstream>

// Access litehtml::html_tag::m_attrs for attribute removal
struct LitehtmlTagAttrsAccess : litehtml::html_tag {
    static auto attrsPtr() { return &LitehtmlTagAttrsAccess::m_attrs; }
};
static const auto kLHAttrsPtr = LitehtmlTagAttrsAccess::attrsPtr();

namespace bro::dom {

// ---------------------------------------------------------------------------
// Node implementations
// ---------------------------------------------------------------------------

void Node::appendChild(Node* child) {
    if (!child) return;

    // Remove from previous parent if any
    if (child->parent_) {
        child->parent_->removeChild(child);
    }

    child->parent_ = this;
    children_.push_back(child);
}

void Node::removeChild(Node* child) {
    if (!child) return;

    auto it = std::find(children_.begin(), children_.end(), child);

    if (it != children_.end()) {
        (*it)->parent_ = nullptr;
        children_.erase(it);
    }
}

void Node::insertBefore(Node* newChild, Node* refChild) {
    if (!newChild) return;

    if (!refChild) {
        appendChild(newChild);
        return;
    }

    // Remove from previous parent if any
    if (newChild->parent_) {
        newChild->parent_->removeChild(newChild);
    }

    auto it = std::find(children_.begin(), children_.end(), refChild);

    if (it != children_.end()) {
        newChild->parent_ = this;
        children_.insert(it, newChild);
    }
}

// ---------------------------------------------------------------------------
// Element implementations
// ---------------------------------------------------------------------------

Element::Element(const std::string& tag)
    : tag_(tag)
    , style_(this)
{
    // Uppercase the tag name (DOM convention)
    for (auto& c : tag_) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
}

std::string Element::id() const {
    return getAttribute("id");
}

void Element::setId(const std::string& val) {
    setAttribute("id", val);
}

std::string Element::className() const {
    return getAttribute("class");
}

void Element::setClassName(const std::string& val) {
    setAttribute("class", val);
}

std::string Element::getAttribute(const std::string& name) const {
    auto it = attributes_.find(name);
    if (it != attributes_.end()) {
        return it->second;
    }
    return {};
}

void Element::setAttribute(const std::string& name, const std::string& val) {
    // Skip if value unchanged — avoids expensive CSS recomputation + layout
    auto existing = attributes_.find(name);
    if (existing != attributes_.end() && existing->second == val) return;
    // If changing ID, unregister old and register new with the document
    if (name == "id" && document_) {
        std::string oldId = getAttribute("id");
        if (!oldId.empty()) document_->unregisterElementId(oldId);
        if (!val.empty()) document_->registerElementId(val, this);
    }
    attributes_[name] = val;
    if (litehtml_element_) {
        litehtml_element_->set_attr(name.c_str(), val.c_str());
        // Class/id changes require CSS re-evaluation so selectors like
        // .active { ... } take effect immediately.
        if (name == "class" || name == "id") {
            litehtml_element_->refresh_styles();
            litehtml_element_->compute_styles(false);
        }
    }
    markDirty();
}

void Element::removeAttribute(const std::string& name) {
    if (name == "id" && document_) {
        std::string oldId = getAttribute("id");
        if (!oldId.empty()) document_->unregisterElementId(oldId);
    }
    attributes_.erase(name);
    // Also remove from litehtml's attribute map so replaced elements see the change
    if (litehtml_element_) {
        auto* htmlTag = dynamic_cast<litehtml::html_tag*>(litehtml_element_.get());
        if (htmlTag) {
            auto& attrs = htmlTag->*kLHAttrsPtr;
            attrs.erase(name);
        }
    }
    markDirty();
}

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
    // Skip if text unchanged — avoids expensive litehtml rebuild + layout
    if (children_.size() == 1 && children_[0]->nodeType() == NodeType::Text) {
        auto* existing = static_cast<TextNode*>(children_[0]);
        if (existing->data() == text) return;
    }
    // Free old children from the document's ownership list before clearing.
    auto oldKids = children_;
    for (auto& child : oldKids) {
        child->setParent(nullptr);
    }
    children_.clear();
    if (document_) {
        for (auto* child : oldKids) {
            document_->freeNode(child);
        }
    }

    // Add a single text node via the owner document (which tracks ownership).
    if (!text.empty() && document_) {
        auto* textNode = document_->createTextNode(text);
        appendChild(textNode);
    }

    // Sync to litehtml so the rendered output updates.
    if (litehtml_element_) {
        bool needsBr = text.find('\n') != std::string::npos &&
                       Document::preservesNewlines(litehtml_element_);

        // Fast path: single text child, no <br> needed — update in-place
        if (!needsBr) {
            auto& lhChildren = litehtml_element_->children();
            if (lhChildren.size() == 1 && lhChildren.front()->is_text()) {
                auto* broText = dynamic_cast<bro_el_text*>(lhChildren.front().get());
                if (broText) {
                    broText->set_text(text.c_str());
                    broText->compute_styles(false);
                    markDirty();
                    return;
                }
            }
        }

        // Slow path: clear litehtml children and rebuild.
        litehtml_element_->clearRecursive();
        auto ri = litehtml_element_->get_render_item();
        if (ri) ri->children().clear();

        if (!text.empty()) {
            auto doc = litehtml_element_->get_document();
            if (doc) {
                if (!needsBr) {
                    // Plain text — single text element
                    auto textEl = std::make_shared<bro_el_text>(text.c_str(), doc);
                    litehtml_element_->appendChild(textEl);
                    textEl->compute_styles(false);
                    if (ri) {
                        auto textRender = textEl->create_render_item(ri);
                        if (textRender) {
                            textRender = textRender->init();
                            ri->add_child(textRender);
                        }
                    }
                } else {
                    // white-space preserves newlines — use HTML with <br>
                    std::string html = Document::textToLitehtmlHtml(text, litehtml_element_);
                    doc->append_children_from_string(
                        *litehtml_element_, html.c_str(), false);
                }
            }
        }
    }

    markDirty();
    markStructureDirty();
}

std::string Element::innerHTML() const {
    std::ostringstream oss;
    for (const auto& child : children_) {
        if (child->nodeType() == NodeType::Text) {
            auto* text = static_cast<const TextNode*>(child);
            oss << text->data();
        } else if (child->nodeType() == NodeType::Comment) {
            auto* comment = static_cast<const CommentNode*>(child);
            oss << "<!--" << comment->data() << "-->";
        } else if (child->nodeType() == NodeType::Element) {
            auto* elem = static_cast<const Element*>(child);
            oss << elem->outerHTML();
        }
    }
    return oss.str();
}

static std::string htmlEscapeAttr(const std::string& val) {
    std::string result;
    result.reserve(val.size());
    for (char c : val) {
        switch (c) {
            case '"':  result += "&quot;"; break;
            case '&':  result += "&amp;"; break;
            case '<':  result += "&lt;"; break;
            case '>':  result += "&gt;"; break;
            default:   result += c; break;
        }
    }
    return result;
}

std::string Element::outerHTML() const {
    std::ostringstream oss;
    std::string lower_tag = tag_;
    for (auto& c : lower_tag) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    oss << "<" << lower_tag;
    for (const auto& [key, val] : attributes_) {
        oss << " " << key << "=\"" << htmlEscapeAttr(val) << "\"";
    }
    // Include inline styles from StyleProxy if not already in attributes
    if (attributes_.find("style") == attributes_.end()) {
        std::string css = style_.cssText();
        if (!css.empty()) {
            oss << " style=\"" << css << "\"";
        }
    }
    oss << ">";
    oss << innerHTML();
    oss << "</" << lower_tag << ">";
    return oss.str();
}

void Element::setInnerHTML(const std::string& html) {
    if (document_) {
        // Delegate to Document which can parse HTML via litehtml
        document_->parseInnerHTML(this, html);
        return;
    }

    // No document — clear children (cannot create owned nodes without a document).
    auto oldKids = children_;
    for (auto& child : oldKids) {
        child->setParent(nullptr);
    }
    children_.clear();
    markDirty();
}

void Element::addEventListener(const std::string& type, uint64_t listenerId) {
    listeners_[type].push_back(listenerId);
}

void Element::removeEventListener(const std::string& type, uint64_t listenerId) {
    auto it = listeners_.find(type);
    if (it != listeners_.end()) {
        auto& vec = it->second;
        vec.erase(std::remove(vec.begin(), vec.end(), listenerId), vec.end());
        if (vec.empty()) {
            listeners_.erase(it);
        }
    }
}

std::vector<Element*> Element::children() const {
    std::vector<Element*> result;
    for (const auto& child : children_) {
        if (child->nodeType() == NodeType::Element) {
            result.push_back(static_cast<Element*>(child));
        }
    }
    return result;
}

Element* Element::parentElement() const {
    if (parent_ && parent_->nodeType() == NodeType::Element) {
        return static_cast<Element*>(parent_);
    }
    return nullptr;
}

std::vector<Element*> Element::querySelectorAll(const std::string& selector) {
    std::vector<Element*> result;

    // Try litehtml first (fast, covers parsed HTML elements)
    if (litehtml_element_ && document_) {
        auto found = litehtml_element_->select_all(selector);
        for (auto& lh : found) {
            // select_all may include `this` element — DOM spec says
            // querySelectorAll only returns descendants, never self.
            if (lh == litehtml_element_) continue;
            auto* elem = document_->findElementByLitehtml(lh);
            if (elem) result.push_back(elem);
        }
    }

    // Search dynamic children only — skip subtrees rooted at litehtml elements
    // since litehtml already searched them above.
    size_t before = result.size();
    querySelectorAllSimple(selector, result);
    if (result.size() > before) {
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
    }

    return result;
}

Element* Element::querySelector(const std::string& selector) {
    // Try litehtml first
    if (litehtml_element_ && document_) {
        auto found = litehtml_element_->select_one(selector);
        if (found) {
            auto* elem = document_->findElementByLitehtml(found);
            if (elem) return elem;
        }
    }

    // Fallback to simple matching
    return querySelectorSimple(selector);
}

bool Element::matches(const std::string& selector) const {
    // Fast path: simple selectors (no combinators) can be checked directly.
    bool isSimple = selector.find_first_of(" >+~,") == std::string::npos;
    if (isSimple) return matchesSimple(selector);

    // Complex selectors need litehtml's CSS engine.
    if (!litehtml_element_ || !document_) return false;
    auto parent = litehtml_element_->parent();
    if (!parent) return false;
    auto found = parent->select_all(selector);
    for (auto& lh : found) {
        if (lh.get() == litehtml_element_.get()) return true;
    }
    return false;
}

Element* Element::closest(const std::string& selector) {
    // Walk up the tree checking matches()
    Element* current = this;
    while (current) {
        if (current->matches(selector)) return current;
        current = current->parentElement();
    }
    return nullptr;
}

// Simple CSS selector matching for dynamically created elements (no litehtml).
// Supports: tag, .class, #id, tag.class, .class1.class2, tag#id
bool Element::matchesSimple(const std::string& selector) const {
    if (selector.empty()) return false;
    // Skip pseudo selectors like :scope
    if (selector[0] == ':') return false;

    // Parse selector into tag, id, and class parts
    std::string reqTag, reqId;
    std::vector<std::string> reqClasses;

    std::string current;
    enum Part { TAG, CLASS, ID } part = TAG;

    for (size_t i = 0; i <= selector.size(); ++i) {
        char c = (i < selector.size()) ? selector[i] : '\0';
        if (c == '.' || c == '#' || c == '\0') {
            if (!current.empty()) {
                if (part == TAG) reqTag = current;
                else if (part == CLASS) reqClasses.push_back(current);
                else if (part == ID) reqId = current;
            }
            current.clear();
            if (c == '.') part = CLASS;
            else if (c == '#') part = ID;
        } else {
            current += c;
        }
    }

    // Match tag (case-insensitive)
    if (!reqTag.empty()) {
        std::string upper = reqTag;
        for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (tag_ != upper) return false;
    }

    // Match id
    if (!reqId.empty() && getAttribute("id") != reqId) return false;

    // Match classes
    if (!reqClasses.empty()) {
        std::string cls = getAttribute("class");
        for (auto& rc : reqClasses) {
            // Check if rc appears as a whole word in cls
            bool found = false;
            std::istringstream iss(cls);
            std::string tok;
            while (iss >> tok) {
                if (tok == rc) { found = true; break; }
            }
            if (!found) return false;
        }
    }
    return true;
}

void Element::querySelectorAllSimple(const std::string& selector, std::vector<Element*>& out) {
    std::string simpleSelector = selector;
    while (!simpleSelector.empty() && simpleSelector.front() == ' ') simpleSelector.erase(0, 1);
    while (!simpleSelector.empty() && simpleSelector.back() == ' ') simpleSelector.pop_back();
    auto spacePos = simpleSelector.rfind(' ');
    if (spacePos != std::string::npos) simpleSelector = simpleSelector.substr(spacePos + 1);
    auto gtPos = simpleSelector.rfind('>');
    if (gtPos != std::string::npos) {
        simpleSelector = simpleSelector.substr(gtPos + 1);
        while (!simpleSelector.empty() && simpleSelector.front() == ' ') simpleSelector.erase(0, 1);
    }

    for (auto& child : children_) {
        if (child->nodeType() == NodeType::Element) {
            auto* elem = static_cast<Element*>(child);
            // Only match dynamic elements (no litehtml counterpart) — litehtml
            // already searched its own elements via select_all above.
            if (!elem->litehtmlElement() && elem->matchesSimple(simpleSelector)) {
                out.push_back(elem);
            }
            elem->querySelectorAllSimple(selector, out);
        }
    }
}

Element* Element::querySelectorSimple(const std::string& selector) {
    std::vector<Element*> results;
    querySelectorAllSimple(selector, results);
    return results.empty() ? nullptr : results[0];
}

void Element::syncStylesToLitehtml(bool displayChanged) {
    if (!litehtml_element_) return;
    std::string css = style_.cssText();
    if (css.empty()) {
        litehtml_element_->set_attr("style", "");
    } else {
        litehtml_element_->set_attr("style", css.c_str());
    }
    if (displayChanged) {
        litehtml_element_->refresh_styles();
    }
    // Use recursive=true so child text nodes inherit updated properties
    // (e.g. color changes on a span must propagate to its text children).
    litehtml_element_->compute_styles(true);
}

void Element::markDirty() {
    dirty_ = true;
    if (document_) {
        document_->markDirty();
    }
}

void Element::markStructureDirty() {
    dirty_ = true;
    if (document_) {
        document_->markStructureDirty();
    }
}

ShadowRoot* Element::containingShadowRoot() const {
    // Walk up the parent chain looking for a ShadowRoot
    for (auto* p = parent_; p; p = p->parentNode()) {
        if (p->nodeType() == NodeType::DocumentFragment) {
            auto* sr = dynamic_cast<ShadowRoot*>(p);
            if (sr) return sr;
        }
    }
    return nullptr;
}

ShadowRoot* Element::attachShadow(ShadowRoot::Mode mode) {
    if (shadowRoot_) return nullptr; // already has shadow root
    if (!document_) return nullptr;

    shadowRoot_ = document_->allocateShadowRoot(this, mode);
    return shadowRoot_;
}

} // namespace bro::dom
