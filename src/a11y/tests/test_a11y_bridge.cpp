#include "a11y/a11y_bridge.h"
#include "dom/document.h"
#include "dom/element.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool is_focusable(const broa11y::Node* n) {
    return n && n->has_state(broa11y::State::Focusable);
}

bool is_focused(const broa11y::Node* n) {
    return n && n->has_state(broa11y::State::Focused);
}

bool is_checked(const broa11y::Node* n) {
    return n && n->has_state(broa11y::State::Checked);
}

bool is_disabled(const broa11y::Node* n) {
    return n && n->has_state(broa11y::State::Disabled);
}

void test_accessibility_bridge_integration() {
    std::cout << "[bro_a11y_test] Starting accessibility bridge integration test...\n";

    // 1. Build a real DOM document with headings, buttons, text inputs, checkboxes, aria attributes
    bro::dom::Document doc;
    const std::string html = R"(<!DOCTYPE html>
<html>
<head><title>Accessibility Test Page</title></head>
<body>
    <main role="main">
        <h1 id="h1" aria-label="Main Accessible Heading">Page Title</h1>
        <h2 id="h2">Section Subheading</h2>
        <button id="btn1">Submit</button>
        <button id="btn2" aria-label="Cancel Operation">Dismiss</button>
        <input type="text" id="txt1" placeholder="Type here..." aria-label="Search Input" value="initial query">
        <input type="checkbox" id="chk1" checked>
        <input type="checkbox" id="chk2" disabled>
        <input type="checkbox" id="chk3" checked disabled>
        <input type="checkbox" id="chk4">
        <div role="button" id="customBtn" tabindex="0" aria-label="Custom Push Button">Custom Button</div>
        <div role="checkbox" id="customChk" tabindex="0" aria-checked="true" aria-label="Custom Checkbox">Custom Checkbox</div>
    </main>
</body>
</html>
)";
    doc.parse(html);

    auto* docEl = doc.documentElement();
    assert(docEl != nullptr && "documentElement should be non-null");

    auto* h1El = doc.getElementById("h1");
    auto* h2El = doc.getElementById("h2");
    auto* btn1El = doc.getElementById("btn1");
    auto* btn2El = doc.getElementById("btn2");
    auto* txt1El = doc.getElementById("txt1");
    auto* chk1El = doc.getElementById("chk1");
    auto* chk2El = doc.getElementById("chk2");
    auto* chk3El = doc.getElementById("chk3");
    auto* chk4El = doc.getElementById("chk4");
    auto* customBtnEl = doc.getElementById("customBtn");
    auto* customChkEl = doc.getElementById("customChk");

    assert(h1El && h2El && btn1El && btn2El && txt1El && chk1El && chk2El && chk3El && chk4El && customBtnEl && customChkEl);

    // 2. Initialize real AccessibilityBridge
    bro::a11y::AccessibilityBridge bridge;
    bool initSuccess = bridge.initialize(&doc);
    assert(initSuccess && "bridge initialize should return true");
    assert(bridge.isRunning() && "bridge should report isRunning == true");

    // 3. Call rebuild()
    bridge.rebuild();

    auto* tree = bridge.tree();
    assert(tree != nullptr && "tree should be non-null");
    assert(tree->root() != nullptr && "tree root should be non-null");

    // 4. Verify the broa11y::Tree matches the DOM structure
    auto rootId = bridge.nodeIdForElement(docEl);
    assert(rootId != broa11y::kInvalidNodeId && "document element should have valid node id");
    assert(tree->root_id() == rootId && "tree root id must match documentElement node id");

    auto* rootNode = tree->root();
    assert(rootNode->role() == broa11y::Role::Document && "root element must have Document role");

    auto* h1Node = bridge.nodeForElement(h1El);
    auto* h2Node = bridge.nodeForElement(h2El);
    auto* btn1Node = bridge.nodeForElement(btn1El);
    auto* btn2Node = bridge.nodeForElement(btn2El);
    auto* txt1Node = bridge.nodeForElement(txt1El);
    auto* chk1Node = bridge.nodeForElement(chk1El);
    auto* chk2Node = bridge.nodeForElement(chk2El);
    auto* chk3Node = bridge.nodeForElement(chk3El);
    auto* chk4Node = bridge.nodeForElement(chk4El);
    auto* customBtnNode = bridge.nodeForElement(customBtnEl);
    auto* customChkNode = bridge.nodeForElement(customChkEl);

    assert(h1Node && h2Node && btn1Node && btn2Node && txt1Node);
    assert(chk1Node && chk2Node && chk3Node && chk4Node);
    assert(customBtnNode && customChkNode);

    // 5. Verify node roles and accessible names
    assert(h1Node->role() == broa11y::Role::Heading);
    assert(h1Node->name() == "Main Accessible Heading");

    assert(h2Node->role() == broa11y::Role::Heading);
    assert(h2Node->name() == "Section Subheading");

    assert(btn1Node->role() == broa11y::Role::Button);
    assert(btn1Node->name() == "Submit");

    assert(btn2Node->role() == broa11y::Role::Button);
    assert(btn2Node->name() == "Cancel Operation");

    assert(txt1Node->role() == broa11y::Role::TextInput);
    assert(txt1Node->name() == "Search Input");

    assert(chk1Node->role() == broa11y::Role::CheckBox);
    assert(chk2Node->role() == broa11y::Role::CheckBox);
    assert(chk3Node->role() == broa11y::Role::CheckBox);
    assert(chk4Node->role() == broa11y::Role::CheckBox);

    assert(customBtnNode->role() == broa11y::Role::Button);
    assert(customBtnNode->name() == "Custom Push Button");

    assert(customChkNode->role() == broa11y::Role::CheckBox);
    assert(customChkNode->name() == "Custom Checkbox");

    // 6. Verify node states: is_focusable, is_focused, is_checked, is_disabled
    assert(is_focusable(btn1Node) && "button should be focusable");
    assert(is_focusable(txt1Node) && "text input should be focusable");
    assert(is_focusable(chk1Node) && "checkbox should be focusable");
    assert(is_focusable(customBtnNode) && "div[tabindex=0] should be focusable");
    assert(!is_focusable(h1Node) && "heading should not be focusable");

    // Checkboxes checked / disabled states
    assert(is_checked(chk1Node) && !is_disabled(chk1Node));
    assert(!is_checked(chk2Node) && is_disabled(chk2Node));
    assert(is_checked(chk3Node) && is_disabled(chk3Node));
    assert(!is_checked(chk4Node) && !is_disabled(chk4Node));
    assert(is_checked(customChkNode) && "custom checkbox with aria-checked=true should be checked");

    // Initial focus state (neither is focused)
    assert(!is_focused(btn1Node));
    assert(!is_focused(txt1Node));

    // 7. Test focus change updates
    std::cout << "[bro_a11y_test] Testing focus changes...\n";
    bridge.onFocusChanged(btn1El);
    assert(tree->focused_node_id() == bridge.nodeIdForElement(btn1El));
    assert(is_focused(btn1Node) && "btn1Node should now be focused");
    assert(!is_focused(txt1Node) && "txt1Node should not be focused");

    bridge.onFocusChanged(txt1El);
    assert(tree->focused_node_id() == bridge.nodeIdForElement(txt1El));
    assert(!is_focused(btn1Node) && "btn1Node focus should be cleared");
    assert(is_focused(txt1Node) && "txt1Node should now be focused");

    bridge.onFocusChanged(nullptr);
    assert(tree->focused_node_id() == broa11y::kInvalidNodeId);
    assert(!is_focused(txt1Node) && "txt1Node focus should be cleared on null focus");

    // 8. Test screen reader announcements
    std::cout << "[bro_a11y_test] Testing screen reader announcements...\n";
    bool politeAnnouncementReceived = false;
    std::string politeMessage;
    broa11y::AnnouncementPriority politePriority = broa11y::AnnouncementPriority::Assertive;

    bool assertiveAnnouncementReceived = false;
    std::string assertiveMessage;
    broa11y::AnnouncementPriority assertivePriority = broa11y::AnnouncementPriority::Polite;

    auto listenerId = tree->add_listener([&](const broa11y::Event& ev) {
        if (ev.type == broa11y::EventType::Announcement) {
            if (const auto* p = ev.get_if<broa11y::AnnouncementPayload>()) {
                if (p->priority == broa11y::AnnouncementPriority::Polite) {
                    politeAnnouncementReceived = true;
                    politeMessage = p->message;
                    politePriority = p->priority;
                } else if (p->priority == broa11y::AnnouncementPriority::Assertive) {
                    assertiveAnnouncementReceived = true;
                    assertiveMessage = p->message;
                    assertivePriority = p->priority;
                }
            }
        }
    });

    bridge.announce("Status: loading complete", false);
    assert(politeAnnouncementReceived && "Polite announcement should be received");
    assert(politeMessage == "Status: loading complete");
    assert(politePriority == broa11y::AnnouncementPriority::Polite);

    bridge.announce("Alert: critical network timeout!", true);
    assert(assertiveAnnouncementReceived && "Assertive announcement should be received");
    assert(assertiveMessage == "Alert: critical network timeout!");
    assert(assertivePriority == broa11y::AnnouncementPriority::Assertive);

    tree->remove_listener(listenerId);

    // 9. Test element change notifications and event processing
    std::cout << "[bro_a11y_test] Testing element change notifications...\n";
    btn1El->setAttribute("aria-label", "New Submit Label");
    bridge.onElementChanged(btn1El);
    assert(btn1Node->name() == "New Submit Label");

    bridge.processEvents();

    // 10. Clean shutdown
    std::cout << "[bro_a11y_test] Testing bridge shutdown...\n";
    bridge.shutdown();
    assert(!bridge.isRunning());
    assert(bridge.tree() == nullptr);

    std::cout << "[bro_a11y_test] All tests PASSED successfully.\n";
}

} // namespace

int main() {
    test_accessibility_bridge_integration();
    return 0;
}
