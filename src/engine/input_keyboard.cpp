#include "engine/engine.h"
#include "engine/input_common.h"
#include "engine/input_editable.h"
#include "engine/settings.h"
#include "engine/key_mapping.h"
#include "platform/clipboard.h"
#include "platform/keys.h"
#include "platform/window.h"
#if BRO_WITH_A11Y
#include "a11y/a11y_bridge.h"
#endif
#include "dom/document.h"
#include "dom/element.h"
#include "dom/range.h"
#include "dom/selection.h"
#include "dom/text_node.h"
#include "layout/control_text.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/el_select.h"
#include "layout/key_handle_result.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <vector>

namespace bro::engine {

void Engine::dispatchFocusEvents(dom::Element* oldTarget, dom::Element* newTarget) {
    if (oldTarget == newTarget) return;

    if (oldTarget) {
        dom::FocusEvent blurEvt("blur", false, false);
        blurEvt.setRelatedTarget(newTarget);
        blurEvt.setIsTrusted(true);
        dispatchEvent(oldTarget, blurEvt);
    }

    if (newTarget) {
        dom::FocusEvent focusEvt("focus", false, false);
        focusEvt.setRelatedTarget(oldTarget);
        focusEvt.setIsTrusted(true);
        dispatchEvent(newTarget, focusEvt);
    }

    if (oldTarget) {
        dom::FocusEvent focusoutEvt("focusout", true, false);
        focusoutEvt.setRelatedTarget(newTarget);
        focusoutEvt.setIsTrusted(true);
        dispatchEvent(oldTarget, focusoutEvt);
    }

    if (newTarget) {
        dom::FocusEvent focusinEvt("focusin", true, false);
        focusinEvt.setRelatedTarget(oldTarget);
        focusinEvt.setIsTrusted(true);
        dispatchEvent(newTarget, focusinEvt);
    }
}

void Engine::handleProgrammaticFocus(dom::Document* doc, dom::Element* oldEl,
                                     dom::Element* newEl) {
    if (!doc || !document_ || doc != document_.get()) return;

    commitActiveComposition();

    if (takeValueChange(oldEl)) {
        dom::ElementHandle keepNew(document_.get(), newEl);
        dom::Event changeEvt("change");
        changeEvt.setIsTrusted(true);
        dispatchEvent(oldEl, changeEvt);
        newEl = keepNew.get();
    }
    armValueChange(newEl);

    if (auto* prevInput = getElInput(oldEl)) prevInput->setFocused(false);
    if (auto* prevTa = getElTextarea(oldEl)) prevTa->setFocused(false);

    if (newEl) ensureReplacedElements(newEl);

    auto* newInput = getElInput(newEl);
    auto* newTa = getElTextarea(newEl);
    if (newInput) {
        newInput->setFocused(true);
        if (newInput->isTextType(newEl)) {
            std::string v = newEl->getAttribute("value");
            newInput->setCursorPos(static_cast<int>(v.size()));
            safeStartTextInput(window_.get());
        } else {
            safeStopTextInput(window_.get());
        }
    } else if (newTa) {
        newTa->setFocused(true);
        std::string v = newEl->hasAttribute("value")
                            ? newEl->getAttribute("value")
                            : newEl->textContent();
        newTa->setCursorPos(static_cast<int>(v.size()));
        safeStartTextInput(window_.get());
    } else if (newEl && (inEditableHost(newEl) || newEl->terminalControl())) {
        safeStartTextInput(window_.get());
    } else {
        safeStopTextInput(window_.get());
    }

    updateTextInputArea();
    markAppBaseDirty();
    uiDirty_ = true;
#if BRO_WITH_A11Y
    if (a11yBridge_) {
        a11yBridge_->onFocusChanged(newEl);
    }
#endif
}

bool Engine::handleGlobalHotkey(int keycode, int mod, bool repeat) {
    if (repeat || !settings_) return false;
    const std::string webKey = sdlKeycodeToWebKey(keycode, mod);
    const std::string action = settings_->getActionForKey(webKey);
    if (action == "system_toggle_perf") {
        toggleSystemPerf();
        uiDirty_ = true;
        return true;
    }
    if (action == "system_toggle_settings") {
        toggleSystemSettings();
        uiDirty_ = true;
        return true;
    }
    if (action == "system_reload_app") {
        requestAppReload();
        return true;
    }
    return false;
}

void Engine::handleKeyDown(int keycode, int scancode, int mod, bool repeat) {
    noteUserActivity();
    if (!modifierBitForKeycode(keycode)) dom::Document::noteInputModality(true);
    heldModifierMask_ |= modifierBitForKeycode(keycode);
    heldKeys_[keycode] = sdlKeycodeToWebKey(keycode, mod);

    // A captured <remoteview> takes every key, ahead of everything of bro's
    // own (input_remote.cpp).
    if (remoteKey(scancode, mod, true, repeat)) return;

    if (overlayMgr_.handleKeyDown(keycode, mod)) {
        uiDirty_ = true;
        return;
    }

    if (inspector_.pickerMode && keycode == platform::kc::Escape && !repeat) {
        inspectorSetPickerMode(false);
        uiDirty_ = true;
        return;
    }

    if (systemSettingsVisible_) {
        bool prevented = systemHandleKeyDown(keycode, scancode, mod, repeat);
        if (!prevented && !repeat) {
            if (keycode == platform::kc::Escape) {
                toggleSystemSettings();
                uiDirty_ = true;
            } else if (settings_) {
                std::string webKey = sdlKeycodeToWebKey(keycode, mod);
                if (settings_->getActionForKey(webKey) == "system_toggle_settings") {
                    toggleSystemSettings();
                    uiDirty_ = true;
                }
            }
        }
        return;
    }

    // A focused <terminal> takes its keys before the page's own handling
    // and the engine's hotkeys (input_terminal.cpp).
    if (terminalKeyDown(keycode, scancode, mod, repeat)) return;

    if (handleGlobalHotkey(keycode, mod, repeat)) return;

    if (!document_) return;

    {
        const bool caretOrCommandKey =
            platform::hasPrimaryMod(mod) ||
            keycode == platform::kc::Left || keycode == platform::kc::Right ||
            keycode == platform::kc::Up || keycode == platform::kc::Down ||
            keycode == platform::kc::Home || keycode == platform::kc::End ||
            keycode == platform::kc::PageUp || keycode == platform::kc::PageDown ||
            keycode == platform::kc::Backspace || keycode == platform::kc::Delete ||
            keycode == platform::kc::Return || keycode == platform::kc::KpEnter ||
            keycode == platform::kc::Tab || keycode == platform::kc::Escape;
        if (caretOrCommandKey) commitActiveComposition();
    }

    if (keycode == platform::kc::Tab) {
        auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
        dom::Element* target = document_->activeElement();
        if (!target) target = document_->body();
        if (target) dispatchEvent(target, evt);

        if (!evt.defaultPrevented()) {
            advanceFocus((mod & platform::kmod::Shift) != 0);
            uiDirty_ = true;
        }
        return;
    }

    // Escape while the top layer holds something: the keydown goes to the
    // focused element first, and unless it is cancelled it is a close request
    // to the topmost entry (a modal dialog: `cancel`, then close).
    if (keycode == platform::kc::Escape && !document_->topLayer().empty()) {
        auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
        dom::Element* target = document_->activeElement();
        if (!target) target = document_->body();
        if (target) dispatchEvent(target, evt);
        if (!evt.defaultPrevented() && !repeat) {
            requestTopLayerClose(document_.get());
            uiDirty_ = true;
        }
        return;
    }

    if (platform::hasPrimaryMod(mod) &&
        (keycode == platform::kc::C || keycode == platform::kc::X || keycode == platform::kc::V)) {

        auto* activeEl = document_->activeElement();
        dom::Element* target = activeEl ? activeEl : document_->body();

        if (keycode == platform::kc::V) {
            std::string text = platform::getClipboardText();

            dom::ClipboardEvent pasteEvt("paste", true, true);
            pasteEvt.setClipboardText(text);
            if (!text.empty()) {
                pasteEvt.addItem({"text/plain", {}, text});
            }
            for (const char* mime : {"image/png", "image/bmp", "image/jpeg"}) {
                if (auto data = platform::clipboard().getData(mime))
                    pasteEvt.addItem({mime, std::move(*data), ""});
            }
            pasteEvt.setIsTrusted(true);
            dispatchEvent(target, pasteEvt);

            if (!pasteEvt.defaultPrevented() && !text.empty()) {
                bool handledByForm = false;
                if (activeEl) {
                    auto* input = getElInput(activeEl);
                    auto* ta = getElTextarea(activeEl);
                    handledByForm = (input && input->isFocused()) ||
                                    (ta && ta->isFocused());
                }
                if (!handledByForm) {
                    auto* sel = document_->selection();
                    if (sel && sel->rangeCount() > 0) {
                        auto* fn = sel->focusNode();
                        if (fn && inEditableHost(fn)) {
                            auto* host = editableHostOf(fn);
                            runEditableMutation(document_.get(), fn,
                                "insertFromPaste", text,
                                [&] {
                                    EditUndoScope undo(&editUndo_, document_.get(), host,
                                                       nullptr, DomUndoStack::Kind::Discrete);
                                    selectionInsertText(document_.get(), text);
                                    undo.commit();
                                });
                            uiDirty_ = true;
                        }
                    }
                }
            }
            if (!pasteEvt.defaultPrevented() && !text.empty() && activeEl) {
                layout::KeyHandleResult r;
                if (auto* input = getElInput(activeEl); input && input->isFocused()) {
                    r = input->pasteText(activeEl, text);
                } else if (auto* textarea = getElTextarea(activeEl); textarea && textarea->isFocused()) {
                    r = textarea->pasteText(activeEl, text);
                }
                if (r.handled) {
                    applyKeyResult(activeEl, r);
                    uiDirty_ = true;
                }
            }
        } else {
            std::string text;
            bool fromFormField = false;
            if (activeEl) {
                if (auto* input = getElInput(activeEl); input && input->isFocused()) {
                    text = input->selectedText();
                    fromFormField = true;
                } else if (auto* textarea = getElTextarea(activeEl); textarea && textarea->isFocused()) {
                    text = textarea->selectedText();
                    fromFormField = true;
                }
            }
            if (!fromFormField) {
                if (auto* sel = document_->selection(); sel && !sel->isCollapsed()) {
                    text = sel->toString();
                }
            }

            std::string evtType = (keycode == platform::kc::C) ? "copy" : "cut";
            dom::ClipboardEvent clipEvt(evtType, true, true);
            clipEvt.setClipboardText(text);
            clipEvt.setIsTrusted(true);
            dispatchEvent(target, clipEvt);

            if (!clipEvt.defaultPrevented() && !text.empty() &&
                platform::setClipboardText(text)) {
                if (keycode == platform::kc::X && fromFormField && activeEl) {
                    bool cut = false;
                    if (auto* input = getElInput(activeEl)) {
                        cut = input->cutSelection(activeEl);
                    } else if (auto* textarea = getElTextarea(activeEl)) {
                        cut = textarea->cutSelection(activeEl);
                    }
                    if (cut) {
                        dom::InputEvent inputEvt("input", true, false);
                        inputEvt.setInputType("deleteByCut");
                        inputEvt.setIsTrusted(true);
                        dispatchEvent(activeEl, inputEvt);
                        if (activeEl->document()) activeEl->document()->markDirty();
                        uiDirty_ = true;
                    }
                } else if (keycode == platform::kc::X && !fromFormField) {
                    auto* sel = document_->selection();
                    if (sel && sel->rangeCount() > 0 && !sel->isCollapsed()) {
                        auto* fn = sel->focusNode();
                        if (fn && inEditableHost(fn)) {
                            auto* host = editableHostOf(fn);
                            runEditableMutation(document_.get(), fn,
                                "deleteByCut", "",
                                [&] {
                                    EditUndoScope undo(&editUndo_, document_.get(), host,
                                                       nullptr, DomUndoStack::Kind::Discrete);
                                    auto* range = sel->getRangeAt(0);
                                    if (!range) return;
                                    dom::Node* after = nullptr; int afterOff = 0;
                                    deleteRangeContents(document_.get(), *range, after, afterOff);
                                    if (after) sel->collapse(after, afterOff);
                                    undo.commit();
                                });
                            uiDirty_ = true;
                        }
                    }
                }
            }
        }
        return;
    }

    auto* activeEl = document_->activeElement();
    layout::KeyHandleResult result;

    // A focused form control: keydown goes to the page FIRST, and the
    // control's default action (Space toggling a checkbox, Backspace editing,
    // arrows stepping a range) runs only when no listener cancelled it — the
    // web's order. It used to act first and dispatch afterwards, so
    // preventDefault() on keydown could not stop it.
    bool keydownDispatched = false;
    auto focusedControl = [this](dom::Element* el) {
        if (auto* in = getElInput(el); in && in->isFocused()) return true;
        if (auto* ta = getElTextarea(el); ta && ta->isFocused()) return true;
        return false;
    };
    if (activeEl && focusedControl(activeEl)) {
        dom::ElementHandle handle(document_.get(), activeEl);
        auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
        evt.setIsComposing(compositionActive());
        dispatchEvent(activeEl, evt);
        keydownDispatched = true;
        if (evt.defaultPrevented()) return;
        // A listener may have moved focus or removed the control.
        activeEl = handle.get();
        if (!activeEl || !document_ || activeEl != document_->activeElement()) return;
        if (auto* input = getElInput(activeEl); input && input->isFocused()) {
            result = input->handleKeyDown(activeEl, keycode, mod);
        } else if (auto* textarea = getElTextarea(activeEl); textarea && textarea->isFocused()) {
            result = textarea->handleKeyDown(activeEl, keycode, mod);
        }
        if (result.handled) {
            applyKeyResult(activeEl, result);
            return;
        }
    }

    if (document_) {
        auto* sel = document_->selection();
        if (sel && sel->rangeCount() > 0) {
            bool shift = (mod & platform::kmod::Shift) != 0;
            bool ctrl = platform::hasPrimaryMod(mod);
            bool handled = false;

            auto moveFocus = [&](dom::Node* n, int off) {
                if (shift) {
                    sel->extend(n, off);
                } else {
                    sel->collapse(n, off);
                }
            };

            dom::Node* focusN = sel->focusNode();
            int focusO = sel->focusOffset();
            auto* focusText = (focusN && focusN->nodeType() == dom::NodeType::Text)
                ? static_cast<dom::TextNode*>(focusN) : nullptr;

            bool editable = focusN && inEditableHost(focusN);

            if (editable && ctrl && (keycode == platform::kc::Z || keycode == platform::kc::Y)) {
                editHistoryStep(/*redo=*/(keycode == platform::kc::Y) || shift);
                handled = true;
            } else if (editable && (keycode == platform::kc::Backspace || keycode == platform::kc::Delete)) {
                editDeleteAtCaret(/*backward=*/keycode == platform::kc::Backspace);
                handled = true;
            } else if (editable && keycode == platform::kc::Return) {
                editInsertLineBreak();
                handled = true;
            } else if (ctrl && keycode == platform::kc::A) {
                editSelectAll();
                handled = true;
            } else if (focusText) {
                const std::string& data = focusText->data();
                int len = static_cast<int>(data.size());
                if (keycode == platform::kc::Left) {
                    if (focusO > 0) {
                        moveFocus(focusText, layout::utf8Prev(data, focusO));
                        handled = true;
                    }
                } else if (keycode == platform::kc::Right) {
                    if (focusO < len) {
                        moveFocus(focusText, layout::utf8Next(data, focusO));
                        handled = true;
                    }
                } else if (keycode == platform::kc::Home) {
                    moveFocus(focusText, 0);
                    handled = true;
                } else if (keycode == platform::kc::End) {
                    moveFocus(focusText, len);
                    handled = true;
                }
            }

            if (handled) {
                markAppBaseDirty();
                if (!keydownDispatched) {
                    auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
                    if (dom::Element* target = document_->activeElement())
                        dispatchEvent(target, evt);
                }
                return;
            }
        }
    }

    if (!keydownDispatched) {
        auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
        evt.setIsComposing(compositionActive());
        dom::Element* target = document_->activeElement();
        if (target) {
            dispatchEvent(target, evt);
        }
    }

    dispatchActionEventForKey(sdlKeycodeToWebKey(keycode, mod), "down", 1.0f);
}

void Engine::handleKeyUp(int keycode, int scancode, int mod, bool repeat) {
    noteUserActivity();
    heldModifierMask_ &= ~modifierBitForKeycode(keycode);
    heldKeys_.erase(keycode);

    if (remoteKey(scancode, mod, false, repeat)) return;

    if (systemSettingsVisible_) {
        systemHandleKeyUp(keycode, scancode, mod, repeat);
        return;
    }
    if (!document_) return;

    if (terminalKeyUp(keycode, scancode, mod, repeat)) {
        dispatchActionEventForKey(sdlKeycodeToWebKey(keycode, mod), "up", 0.0f);
        return;
    }

    auto evt = makeKeyboardEvent("keyup", keycode, scancode, mod, repeat);
    evt.setIsComposing(compositionActive());

    dom::Element* target = document_->activeElement();
    if (target) {
        dispatchEvent(target, evt);
    }

    dispatchActionEventForKey(sdlKeycodeToWebKey(keycode, mod), "up", 0.0f);
}

void Engine::advanceFocus(bool reverse) {
    if (!document_) return;

    commitActiveComposition();

    std::vector<dom::Element*> focusable;
    auto* body = document_->body();
    if (!body) return;

    std::function<void(dom::Node*)> walk = [&](dom::Node* node) {
        if (!node) return;
        if (node->nodeType() == dom::NodeType::Element) {
            auto* el = static_cast<dom::Element*>(node);
            std::string tag = el->tagName();
            for (auto& c : tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            bool isFocusable = (tag == "input" || tag == "textarea" || tag == "select" || tag == "button" ||
                                tag == "terminal" || tag == "remoteview");
            if (isFocusable) {
                auto* inp = getElInput(el);
                if (inp && inp->inputType(el) == layout::ElInput::InputType::Hidden)
                    isFocusable = false;
                if (el->getAttribute("disabled") == "true" || el->attributes().count("disabled"))
                    isFocusable = false;
            }
            // Outside a modal dialog (the top layer's blocking element) the
            // page is inert: Tab cycles inside the dialog only.
            if (isFocusable && document_->isInert(el)) isFocusable = false;
            if (isFocusable) focusable.push_back(el);
        }
        for (auto* child : node->childNodes()) walk(child);
    };
    walk(body);

    if (focusable.empty()) return;

    auto* activeEl = document_->activeElement();
    int currentIdx = -1;
    for (int i = 0; i < static_cast<int>(focusable.size()); ++i) {
        if (focusable[i] == activeEl) { currentIdx = i; break; }
    }

    int nextIdx;
    if (reverse) {
        nextIdx = (currentIdx <= 0) ? static_cast<int>(focusable.size()) - 1 : currentIdx - 1;
    } else {
        nextIdx = (currentIdx < 0 || currentIdx >= static_cast<int>(focusable.size()) - 1) ? 0 : currentIdx + 1;
    }

    auto* nextEl = focusable[nextIdx];

    if (activeEl) {
        if (takeValueChange(activeEl)) {
            dom::ElementHandle keepNext(document_.get(), nextEl);
            dom::Event changeEvt("change");
            changeEvt.setIsTrusted(true);
            dispatchEvent(activeEl, changeEvt);
            nextEl = keepNext.get();
            if (!nextEl) { uiDirty_ = true; return; }
        }
        auto* prevInput = getElInput(activeEl);
        if (prevInput) prevInput->setFocused(false);
        auto* prevTa = getElTextarea(activeEl);
        if (prevTa) prevTa->setFocused(false);
        if (getElSelect(activeEl)) overlayMgr_.close();
    }

    document_->setActiveElement(nextEl);
    dispatchFocusEvents(activeEl, nextEl);
    armValueChange(nextEl);
#if BRO_WITH_A11Y
    if (a11yBridge_) {
        a11yBridge_->onFocusChanged(nextEl);
    }
#endif

    auto* newInput = getElInput(nextEl);
    auto* newTa = getElTextarea(nextEl);

    if (newInput) {
        newInput->setFocused(true);
        if (newInput->isTextType(nextEl)) {
            std::string v = nextEl->getAttribute("value");
            newInput->setCursorPos(static_cast<int>(v.size()));
            safeStartTextInput(window_.get());
        } else {
            safeStopTextInput(window_.get());
        }
    } else if (newTa) {
        newTa->setFocused(true);
        std::string v = nextEl->getAttribute("value");
        newTa->setCursorPos(static_cast<int>(v.size()));
        safeStartTextInput(window_.get());
    } else if (nextEl->terminalControl()) {
        safeStartTextInput(window_.get());
    } else {
        safeStopTextInput(window_.get());
    }

    updateTextInputArea();
    uiDirty_ = true;
}

} // namespace bro::engine
