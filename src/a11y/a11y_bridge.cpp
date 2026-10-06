#include "a11y/a11y_bridge.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"

#include <cctype>

namespace bro::a11y {

AccessibilityBridge::AccessibilityBridge() = default;

AccessibilityBridge::~AccessibilityBridge() {
    shutdown();
}

bool AccessibilityBridge::initialize(dom::Document* document) {
    if (!document) return false;
    document_ = document;

#if BRO_WITH_A11Y
    tree_ = std::make_unique<broa11y::Tree>();
#if defined(__linux__)
    bridge_ = std::make_unique<broa11y::LinuxBridge>();
#elif defined(_WIN32)
    bridge_ = std::make_unique<broa11y::WinBridge>();
#elif defined(__APPLE__)
    bridge_ = std::make_unique<broa11y::MacBridge>();
#endif
    if (bridge_) {
        bridge_->initialize(tree_.get());
    }
    initialized_ = true;
    updateTree();
    return true;
#else
    initialized_ = true;
    return true;
#endif
}

void AccessibilityBridge::shutdown() {
#if BRO_WITH_A11Y
    if (bridge_) {
        bridge_->shutdown();
        bridge_.reset();
    }
    tree_.reset();
    elementMap_.clear();
#endif
    document_ = nullptr;
    initialized_ = false;
}

#if BRO_WITH_A11Y
static std::string lowerTag(const dom::Element* el) {
    if (!el) return "";
    std::string tag = el->tagName();
    for (char& c : tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return tag;
}

broa11y::Role AccessibilityBridge::mapElementRole(const dom::Element* el) const {
    if (!el) return broa11y::Role::Unknown;

    if (el->hasAttribute("role")) {
        const std::string& roleStr = el->getAttribute("role");
        if (roleStr == "textbox") return broa11y::Role::TextInput;
        if (roleStr == "checkbox") return broa11y::Role::CheckBox;
        if (roleStr == "main") return broa11y::Role::Section;
        auto r = broa11y::string_to_role(roleStr);
        if (r != broa11y::Role::Unknown) return r;
    }

    const std::string tag = lowerTag(el);
    if (tag == "button") return broa11y::Role::Button;
    if (tag == "a") return broa11y::Role::Link;
    if (tag == "input") {
        std::string type = el->getAttribute("type");
        if (type == "checkbox") return broa11y::Role::CheckBox;
        if (type == "radio") return broa11y::Role::RadioButton;
        if (type == "range") return broa11y::Role::Slider;
        return broa11y::Role::TextInput;
    }
    if (tag == "textarea") return broa11y::Role::TextInput;
    if (tag == "select") return broa11y::Role::ComboBox;
    if (tag == "option") return broa11y::Role::MenuItem;
    if (tag == "dialog") return broa11y::Role::Dialog;
    if (tag == "terminal") return broa11y::Role::Terminal;
    if (tag == "h1" || tag == "h2" || tag == "h3" ||
        tag == "h4" || tag == "h5" || tag == "h6") return broa11y::Role::Heading;
    if (tag == "ul" || tag == "ol") return broa11y::Role::List;
    if (tag == "li") return broa11y::Role::ListItem;
    if (tag == "img") return broa11y::Role::Image;
    if (tag == "canvas") return broa11y::Role::Canvas;
    if (tag == "table") return broa11y::Role::Table;
    if (tag == "tr") return broa11y::Role::TableRow;
    if (tag == "td" || tag == "th") return broa11y::Role::TableCell;
    if (tag == "nav" || tag == "aside") return broa11y::Role::Panel;
    if (tag == "section" || tag == "main" || tag == "header" || tag == "footer") return broa11y::Role::Section;
    if (tag == "html" || tag == "body") return broa11y::Role::Document;

    return broa11y::Role::Group;
}

std::string AccessibilityBridge::computeAccessibleName(const dom::Element* el) const {
    if (!el) return "";
    if (el->hasAttribute("aria-label")) return el->getAttribute("aria-label");
    if (el->hasAttribute("aria-labelledby")) {
        const std::string& labelId = el->getAttribute("aria-labelledby");
        if (document_) {
            if (const auto* labelEl = document_->getElementById(labelId)) {
                return labelEl->textContent();
            }
        }
    }
    if (el->hasAttribute("title")) return el->getAttribute("title");
    if (el->hasAttribute("alt")) return el->getAttribute("alt");
    if (el->hasAttribute("placeholder")) return el->getAttribute("placeholder");

    const std::string tag = lowerTag(el);
    if (tag == "button" || tag == "a" || tag == "label" ||
        tag == "h1" || tag == "h2" || tag == "h3" ||
        tag == "h4" || tag == "h5" || tag == "h6") {
        return el->textContent();
    }
    return "";
}

void AccessibilityBridge::updateElementState(dom::Element* el, broa11y::Node* a11yNode) {
    if (!el || !a11yNode) return;
    const std::string tag = lowerTag(el);
    a11yNode->set_state(broa11y::State::Focusable,
        el->hasAttribute("tabindex") ||
        tag == "button" ||
        tag == "input" ||
        tag == "textarea" ||
        tag == "a" ||
        tag == "select");

    bool isFocused = false;
    if (tree_ && tree_->focused_node_id() == a11yNode->id()) {
        isFocused = true;
    } else if (document_ && document_->activeElement() == el &&
               tag != "body" && tag != "html") {
        isFocused = true;
    }
    a11yNode->set_state(broa11y::State::Focused, isFocused);

    bool disabled = el->hasAttribute("disabled");
    if (el->hasAttribute("aria-disabled")) {
        disabled = (el->getAttribute("aria-disabled") == "true");
    }
    a11yNode->set_state(broa11y::State::Disabled, disabled);

    bool checked = el->hasAttribute("checked");
    if (el->hasAttribute("aria-checked")) {
        checked = (el->getAttribute("aria-checked") == "true");
    }
    a11yNode->set_state(broa11y::State::Checked, checked);
}

void AccessibilityBridge::syncElementSubtree(dom::Element* el, broa11y::NodeId parentId) {
    if (!el) return;

    if (el->hasAttribute("aria-hidden") && el->getAttribute("aria-hidden") == "true") {
        return;
    }

    const std::string tag = lowerTag(el);
    if (tag == "head" || tag == "script" || tag == "style" ||
        tag == "meta" || tag == "link" || tag == "template") {
        return;
    }

    auto role = mapElementRole(el);
    auto it = elementMap_.find(el);
    broa11y::NodeId nodeId = (it != elementMap_.end()) ? it->second : tree_->allocate_id();
    elementMap_[el] = nodeId;

    broa11y::Node* node = tree_->get_node(nodeId);
    if (!node) {
        node = tree_->create_node_with_role(role, nodeId);
    } else {
        node->set_role(role);
    }

    if (node) {
        node->set_name(computeAccessibleName(el));
        auto box = dom::absoluteBorderBox(el);
        node->set_bounds(broa11y::RectF{box.x, box.y, box.width, box.height});
        updateElementState(el, node);

        if (parentId != broa11y::kInvalidNodeId) {
            tree_->reparent_node(nodeId, parentId);
        }
    }

    for (auto* child : el->children()) {
        syncElementSubtree(child, nodeId);
    }
}
#endif

void AccessibilityBridge::updateTree() {
#if BRO_WITH_A11Y
    if (!document_ || !tree_) return;
    tree_->begin_transaction();

    auto* rootEl = document_->documentElement();
    if (rootEl) {
        syncElementSubtree(rootEl, broa11y::kInvalidNodeId);
        auto it = elementMap_.find(rootEl);
        if (it != elementMap_.end()) {
            tree_->set_root_id(it->second);
        }
    }

    tree_->commit_transaction();
    if (bridge_) bridge_->process_events();
#endif
}

void AccessibilityBridge::rebuild() {
    updateTree();
}

void AccessibilityBridge::onFocusChanged(dom::Element* focusedElement) {
#if BRO_WITH_A11Y
    if (!tree_) return;
    if (focusedElement) {
        auto it = elementMap_.find(focusedElement);
        if (it != elementMap_.end()) {
            tree_->set_focus(it->second);
        }
    } else {
        tree_->clear_focus();
    }
    if (bridge_) bridge_->process_events();
#else
    (void)focusedElement;
#endif
}

void AccessibilityBridge::onElementChanged(dom::Element* element) {
#if BRO_WITH_A11Y
    if (!tree_ || !element) return;
    auto it = elementMap_.find(element);
    if (it != elementMap_.end()) {
        broa11y::Node* node = tree_->get_node(it->second);
        if (node) {
            node->set_name(computeAccessibleName(element));
            auto box = dom::absoluteBorderBox(element);
            node->set_bounds(broa11y::RectF{box.x, box.y, box.width, box.height});
            updateElementState(element, node);
        }
    }
    if (bridge_) bridge_->process_events();
#else
    (void)element;
#endif
}

void AccessibilityBridge::announce(std::string_view message, bool assertive) {
#if BRO_WITH_A11Y
    if (!tree_) return;
    tree_->announce(message, assertive
        ? broa11y::AnnouncementPriority::Assertive
        : broa11y::AnnouncementPriority::Polite);
    if (bridge_) bridge_->process_events();
#else
    (void)message;
    (void)assertive;
#endif
}

void AccessibilityBridge::processEvents() {
#if BRO_WITH_A11Y
    if (bridge_) {
        bridge_->process_events();
    }
#endif
}

} // namespace bro::a11y
