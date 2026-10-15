#include "a11y/a11y_bridge.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/accessibility.h"

#if BRO_WITH_A11Y
#include <broa11y/api.h>
#endif

#include <cctype>

namespace bro::a11y {

AccessibilityBridge::AccessibilityBridge() = default;

AccessibilityBridge::~AccessibilityBridge() {
    shutdown();
}

bool AccessibilityBridge::initialize(dom::Document* document, void* nativeWindow) {
    if (!document) return false;
    document_ = document;

#if BRO_WITH_A11Y
    tree_ = std::make_shared<broa11y::Tree>();
#if defined(__linux__)
    (void)nativeWindow;
    bridge_ = std::make_shared<broa11y::LinuxBridge>();
#elif defined(_WIN32)
    broa11y::WinBridgeConfig cfg;
    cfg.hwnd = nativeWindow;
    bridge_ = std::make_shared<broa11y::WinBridge>(cfg);
#elif defined(__APPLE__)
    (void)nativeWindow;
    bridge_ = std::make_shared<broa11y::MacBridge>();
#else
    (void)nativeWindow;
#endif
    if (bridge_) {
        bridge_->initialize(tree_.get());
    }
    broa11y::api::setTree(tree_);
    broa11y::api::setBridge(bridge_);
    initialized_ = true;
    updateTree();
    return true;
#else
    (void)nativeWindow;
    initialized_ = true;
    return true;
#endif
}

void AccessibilityBridge::shutdown() {
#if BRO_WITH_A11Y
    if (initialized_ || bridge_ || tree_) {
        broa11y::api::setBridge(nullptr);
        broa11y::api::setTree(nullptr);
    }
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
    // The ARIA role (dom/accessibility.h, shared with the agent view), in
    // broa11y's vocabulary.
    const std::string r = dom::accessibleRole(el);
    if (r.empty()) return broa11y::Role::Group;
    if (r == "textbox" || r == "searchbox" || r == "spinbutton") return broa11y::Role::TextInput;
    if (r == "radio") return broa11y::Role::RadioButton;
    if (r == "option") return broa11y::Role::MenuItem;
    if (r == "listitem") return broa11y::Role::ListItem;
    if (r == "img") return broa11y::Role::Image;
    if (r == "row") return broa11y::Role::TableRow;
    if (r == "cell" || r == "columnheader" || r == "rowheader") return broa11y::Role::TableCell;
    if (r == "navigation" || r == "complementary") return broa11y::Role::Panel;
    if (r == "main" || r == "region" || r == "banner" || r == "contentinfo") return broa11y::Role::Section;
    if (r == "progressbar" || r == "meter") return broa11y::Role::ProgressBar;
    if (r == "status") return broa11y::Role::StatusBar;
    if (r == "switch") return broa11y::Role::CheckBox;
    if (r == "menuitemcheckbox" || r == "menuitemradio") return broa11y::Role::MenuItem;
    auto role = broa11y::string_to_role(r);
    return role != broa11y::Role::Unknown ? role : broa11y::Role::Group;
}

std::string AccessibilityBridge::computeAccessibleName(const dom::Element* el) const {
    return dom::accessibleName(el, document_);
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
