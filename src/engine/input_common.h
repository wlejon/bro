#pragma once

#include "dom/event.h"
#include "dom/event_dispatch.h"
#include "platform/sdl_window.h"

#include <string>

#include "dom/node.h"
#include "dom/element.h"
#include "dom/document.h"

namespace bro::dom {
class Range;
} // namespace bro::dom

namespace htmlayout::css { class Cascade; }

namespace bro::engine {

void safeStartTextInput(platform::Window* window);
void safeStopTextInput(platform::Window* window);

int modifierBitForKeycode(int keycode);

bool isCaretControl(dom::Element* el);

dom::KeyboardEvent makeKeyboardEvent(const char* type, int keycode, int scancode,
                                     int mod, bool repeat);

int sdlToDomButton(int sdlButton);

int domButtonMask(int domButton);

void populateMouseEvent(dom::MouseEvent& evt, float x, float y,
                        int button, int buttons,
                        float movementX, float movementY,
                        float scrollY, int mod,
                        float contentTop = 0.0f);

platform::CursorShape cursorShapeFromCss(const std::string& value);
const char* cursorShapeName(platform::CursorShape s);

bool isControlChar(const std::string& text);

dom::Element* editableHostOf(dom::Node* node);
bool inEditableHost(dom::Node* node);
bool isSelectionSuppressed(dom::Element* el);
void markHoverChainDirty(const htmlayout::css::Cascade& cascade,
                         dom::Element* prev, dom::Element* target);
void deleteRangeContents(dom::Document* doc, dom::Range& r,
                         dom::Node*& node, int& off);

template <typename EditFn>
void runEditableMutation(dom::Document* doc,
                         dom::Node* focusNode,
                         const std::string& inputType,
                         const std::string& data,
                         EditFn&& runEdit) {
    if (!doc || !focusNode) return;
    auto* host = focusNode;
    while (host && host->nodeType() != dom::NodeType::Element)
        host = host->parentNode();
    auto* hostEl = host ? static_cast<dom::Element*>(host) : doc->body();

    dom::InputEvent beforeEvt("beforeinput", /*bubbles=*/true, /*cancelable=*/true);
    beforeEvt.setInputType(inputType);
    beforeEvt.setData(data);
    beforeEvt.setIsTrusted(true);
    if (hostEl) {
        dom::dispatchDomEvent(hostEl, beforeEvt);
    }
    if (beforeEvt.defaultPrevented()) return;

    runEdit();

    dom::InputEvent inputEvt("input", /*bubbles=*/true, /*cancelable=*/false);
    inputEvt.setInputType(inputType);
    inputEvt.setData(data);
    inputEvt.setIsTrusted(true);
    if (hostEl) {
        dom::dispatchDomEvent(hostEl, inputEvt);
    }
    doc->markDirty();
}

} // namespace bro::engine
