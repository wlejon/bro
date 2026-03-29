#include "dom/element.h"
#include "dom/document.h"
#include "dom/text_node.h"
#include <litehtml.h>
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
    // Remove all existing children
    for (auto& child : children_) {
        child->setParent(nullptr);
    }
    children_.clear();

    // Add a single text node
    if (!text.empty()) {
        auto textNode = std::make_shared<TextNode>(text);
        appendChild(std::move(textNode));
    }

    // Sync to litehtml so the rendered output updates
    if (litehtml_element_) {
        // Only call append_children_from_string if the element has a render item,
        // otherwise litehtml will crash dereferencing a null render_item pointer.
        auto renderItem = litehtml_element_->get_render_item();
        if (renderItem) {
            auto doc = litehtml_element_->get_document();
            if (doc) {
                doc->append_children_from_string(*litehtml_element_, text.c_str(), true);
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
            // Opening tag
            std::string lower_tag = elem->tagName();
            for (auto& c : lower_tag) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            oss << "<" << lower_tag;
            for (const auto& [key, val] : elem->attributes_) {
                oss << " " << key << "=\"" << val << "\"";
            }
            oss << ">";
            oss << elem->innerHTML();
            oss << "</" << lower_tag << ">";
        }
    }
    return oss.str();
}

void Element::setInnerHTML(const std::string& html) {
    // Remove all existing children
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

void Element::syncStylesToLitehtml() {
    if (!litehtml_element_) return;
    std::string css = style_.cssText();
    if (css.empty()) {
        litehtml_element_->set_attr("style", "");
    } else {
        litehtml_element_->set_attr("style", css.c_str());
    }
    // Clear and re-apply stylesheet rules, then add inline style on top.
    // This ensures inline styles override correctly without accumulation.
    litehtml_element_->refresh_styles();
    litehtml_element_->compute_styles(false);
}

void Element::markDirty() {
    dirty_ = true;
    if (document_) {
        document_->markDirty();
    }
}

} // namespace bro::dom
