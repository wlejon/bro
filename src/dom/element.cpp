#include "dom/element.h"
#include "dom/document.h"
#include "dom/text_node.h"
#include <litehtml.h>
#include <litehtml/el_text.h>
#include <litehtml/render_item.h>

namespace {

/// el_text subclass with a public text setter.
/// Allows updating text content without destroying the render item.
class bro_el_text : public litehtml::el_text {
public:
    using el_text::el_text;  // inherit constructor

    void set_text(const char* text) {
        m_text = text ? text : "";
        m_use_transformed = false;
    }
};

} // anonymous namespace
#include <algorithm>
#include <sstream>

namespace bro::dom {

// ---------------------------------------------------------------------------
// Node implementations
// ---------------------------------------------------------------------------

void Node::appendChild(std::shared_ptr<Node> child) {
    if (!child) return;

    // Remove from previous parent if any
    if (child->parent_) {
        child->parent_->removeChild(child.get());
    }

    child->parent_ = this;
    children_.push_back(std::move(child));
}

void Node::removeChild(Node* child) {
    if (!child) return;

    auto it = std::find_if(children_.begin(), children_.end(),
        [child](const std::shared_ptr<Node>& n) { return n.get() == child; });

    if (it != children_.end()) {
        (*it)->parent_ = nullptr;
        children_.erase(it);
    }
}

void Node::insertBefore(std::shared_ptr<Node> newChild, Node* refChild) {
    if (!newChild) return;

    if (!refChild) {
        appendChild(std::move(newChild));
        return;
    }

    // Remove from previous parent if any
    if (newChild->parent_) {
        newChild->parent_->removeChild(newChild.get());
    }

    auto it = std::find_if(children_.begin(), children_.end(),
        [refChild](const std::shared_ptr<Node>& n) { return n.get() == refChild; });

    if (it != children_.end()) {
        newChild->parent_ = this;
        children_.insert(it, std::move(newChild));
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
    markDirty();
}

std::string Element::textContent() const {
    std::string result;
    for (const auto& child : children_) {
        if (child->nodeType() == NodeType::Text) {
            auto* text = static_cast<const TextNode*>(child.get());
            result += text->data();
        } else if (child->nodeType() == NodeType::Element) {
            auto* elem = static_cast<const Element*>(child.get());
            result += elem->textContent();
        }
    }
    return result;
}

void Element::setTextContent(const std::string& text) {
    // Skip if text unchanged — avoids expensive litehtml rebuild + layout
    if (children_.size() == 1 && children_[0]->nodeType() == NodeType::Text) {
        auto* existing = static_cast<TextNode*>(children_[0].get());
        if (existing->data() == text) return;
    }
    for (auto& child : children_) {
        child->setParent(nullptr);
    }
    children_.clear();

    // Add a single text node
    if (!text.empty()) {
        auto textNode = std::make_shared<TextNode>(text);
        appendChild(std::move(textNode));
    }

    // Sync to litehtml so the rendered output updates.
    // We use bro_el_text (subclass of el_text with public setter) to update
    // text in-place without destroying render items.  This avoids flicker
    // from the destroy/recreate cycle of append_children_from_string.
    if (litehtml_element_) {
        auto& lhChildren = litehtml_element_->children();

        // Fast path: single text child — update its text in-place
        if (lhChildren.size() == 1 && lhChildren.front()->is_text()) {
            auto* broText = dynamic_cast<bro_el_text*>(lhChildren.front().get());
            if (broText) {
                broText->set_text(text.c_str());
                broText->compute_styles(false);
                markDirty();
                return;
            }
        }

        // Slow path: clear children and create a new bro_el_text
        litehtml_element_->clearRecursive();
        auto ri = litehtml_element_->get_render_item();
        if (ri) {
            ri->children().clear();
        }

        if (!text.empty()) {
            auto doc = litehtml_element_->get_document();
            if (doc) {
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
            }
        }
    }

    markDirty();
}

std::string Element::innerHTML() const {
    std::ostringstream oss;
    for (const auto& child : children_) {
        if (child->nodeType() == NodeType::Text) {
            auto* text = static_cast<const TextNode*>(child.get());
            oss << text->data();
        } else if (child->nodeType() == NodeType::Element) {
            auto* elem = static_cast<const Element*>(child.get());
            oss << elem->outerHTML();
        }
    }
    return oss.str();
}

std::string Element::outerHTML() const {
    std::ostringstream oss;
    std::string lower_tag = tag_;
    for (auto& c : lower_tag) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    oss << "<" << lower_tag;
    for (const auto& [key, val] : attributes_) {
        oss << " " << key << "=\"" << val << "\"";
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
    for (auto& child : children_) {
        child->setParent(nullptr);
    }
    children_.clear();

    // For now, store as a single text node with raw HTML.
    // Full parsing is handled at the Document level via reparse().
    if (!html.empty()) {
        auto textNode = std::make_shared<TextNode>(html);
        appendChild(std::move(textNode));
    }
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
            result.push_back(static_cast<Element*>(child.get()));
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
    if (!litehtml_element_ || !document_) return false;

    // Check if this element appears in parent's select_all results
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
            auto* elem = static_cast<Element*>(child.get());
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
        // display changes need refresh_styles() to re-evaluate stylesheet
        // rules (e.g. removing inline display:none must restore the
        // stylesheet display value).  This also triggers render tree rebuild.
        litehtml_element_->refresh_styles();
    }
    litehtml_element_->compute_styles(false);
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

} // namespace bro::dom
