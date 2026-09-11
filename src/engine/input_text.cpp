#include "engine/engine.h"
#include "engine/input_common.h"
#include "engine/input_editable.h"
#include "platform/clipboard.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/event.h"
#include "dom/range.h"
#include "dom/selection.h"
#include "dom/text_node.h"
#include "layout/control_text.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/key_handle_result.h"
#include "layout/selection_geometry.h"
#include "layout/skia_text_metrics.h"
#include "util/time.h"
#include "platform/sdl_window.h"

#include <SDL3/SDL.h>
#include <cmath>
#include <cstring>

namespace bro::engine {

void Engine::dispatchInputEvent(dom::Element* el, const std::string& data,
                                const std::string& inputType, bool isComposing) {
    if (!el) return;
    dom::InputEvent evt("input");
    evt.setData(data);
    evt.setInputType(inputType);
    evt.setIsComposing(isComposing);
    evt.setIsTrusted(true);
    dispatchEvent(el, evt);
    uiDirty_ = true;
}

void Engine::applyKeyResult(dom::Element* el, const layout::KeyHandleResult& r) {
    if (r.dispatchChange) {
        dom::Event changeEvt("change");
        dispatchEvent(el, changeEvt);
    }
    if (r.dispatchInput) {
        dispatchInputEvent(el, r.inputData, r.inputType);
    }
    if (r.unfocus) {
        if (takeValueChange(el)) {
            dom::Event changeEvt("change");
            changeEvt.setIsTrusted(true);
            dispatchEvent(el, changeEvt);
        }
        dispatchFocusEvents(el, nullptr);
        if (document_) document_->setActiveElement(nullptr);
        safeStopTextInput(window_.get());
    }
    if (r.handled) {
        markAppBaseDirty();
        updateTextInputArea();
    }
}

bool Engine::compositionActive() {
    auto* el = document_ ? document_->activeElement() : nullptr;
    if (auto* input = getElInput(el); input && input->isComposing()) return true;
    if (auto* ta = getElTextarea(el); ta && ta->isComposing()) return true;
    if (editComp_.active) return true;
    return false;
}

dom::TextNode* Engine::editableCompositionTarget() {
    if (!editComp_.active) return nullptr;
    auto* tn = editComp_.node.get();
    if (!document_ || !tn || !document_->ownsNode(tn)) {
        editComp_ = {};
        return nullptr;
    }
    const std::string& data = tn->data();
    if (editComp_.start < 0 || editComp_.length < 0 ||
        static_cast<size_t>(editComp_.start) + static_cast<size_t>(editComp_.length) > data.size() ||
        data.compare(static_cast<size_t>(editComp_.start),
                     static_cast<size_t>(editComp_.length),
                     editComp_.preedit) != 0) {
        editComp_ = {};
        return nullptr;
    }
    return tn;
}

bool Engine::editableCompositionUpdate(const std::string& text, int cursorCp,
                                       bool& wasComposing,
                                       std::string& replacedSel,
                                       dom::Element*& hostOut) {
    wasComposing = editComp_.active;
    replacedSel.clear();
    hostOut = nullptr;
    if (!document_) return false;
    auto* sel = document_->selection();
    if (!sel) return false;

    if (editComp_.active) {
        auto* tn = editableCompositionTarget();
        if (!tn) {
            wasComposing = false;
            return false;
        }
        tn->replaceData(static_cast<size_t>(editComp_.start),
                        static_cast<size_t>(editComp_.length), text);
        editComp_.length = static_cast<int>(text.size());
        editComp_.preedit = text;
        sel->collapse(tn, editComp_.start +
                              layout::utf8ByteForCodepoint(text, cursorCp));
        hostOut = editComp_.host.get();
        document_->markDirty();
        return true;
    }

    auto* focusNode = sel->rangeCount() > 0 ? sel->focusNode() : nullptr;
    auto* host = focusNode ? editableHostOf(focusNode) : nullptr;
    if (!host) return false;

    if (!sel->isCollapsed()) replacedSel = sel->toString();

    const std::string hostBefore = host->innerHTML();
    const auto compSelBefore = DomUndoStack::selectionOf(document_.get(), host);

    int off = 0;
    bool created = false;
    auto* tn = selectionCaretTextPosition(document_.get(), off, created);
    if (!tn) return false;

    editComp_.hostBefore = hostBefore;
    editComp_.selBefore = compSelBefore;
    editComp_.replacedSelection = !replacedSel.empty();
    editComp_.active = true;
    editComp_.node.assign(document_.get(), tn);
    editComp_.host.assign(document_.get(), host);
    editComp_.createdNode = created;
    editComp_.start = off;
    tn->insertData(static_cast<size_t>(off), text);
    editComp_.length = static_cast<int>(text.size());
    editComp_.preedit = text;
    sel->collapse(tn, off + layout::utf8ByteForCodepoint(text, cursorCp));
    hostOut = host;
    document_->markDirty();
    return true;
}

bool Engine::editableCompositionCommit(const std::string& text,
                                       dom::Element*& hostOut, bool cancel) {
    hostOut = nullptr;
    if (!editComp_.active) return false;
    auto* host = editComp_.host.get();
    auto* tn = editableCompositionTarget();
    if (!tn) return false;
    const int start = editComp_.start;
    const int length = editComp_.length;
    const bool createdNode = editComp_.createdNode;
    const std::string hostBefore = editComp_.hostBefore;
    const auto selBefore = editComp_.selBefore;
    const bool replacedSelection = editComp_.replacedSelection;
    editComp_ = {};

    if (cancel && replacedSelection && host) {
        host->setInnerHTML(hostBefore);
        host->markStructureDirty();
        DomUndoStack::restoreSelection(document_.get(), host, selBefore);
        document_->markDirty();
        hostOut = host;
        return true;
    }

    tn->replaceData(static_cast<size_t>(start), static_cast<size_t>(length), text);
    auto* sel = document_->selection();
    if (tn->length() == 0 && createdNode && tn->parentNode()) {
        auto* parent = tn->parentNode();
        const auto& kids = parent->childNodes();
        int idx = 0;
        for (size_t i = 0; i < kids.size(); ++i) {
            if (kids[i] == tn) { idx = static_cast<int>(i); break; }
        }
        parent->removeChild(tn);
        if (parent->nodeType() == dom::NodeType::Element)
            static_cast<dom::Element*>(parent)->markStructureDirty();
        if (sel) sel->collapse(parent, idx);
    } else if (sel) {
        sel->collapse(tn, start + static_cast<int>(text.size()));
    }

    if (!cancel && host) {
        editUndo_.forHost(document_.get(), host)
            .recordStructural(host, hostBefore, host->innerHTML(), selBefore,
                              DomUndoStack::selectionOf(document_.get(), host),
                              util::currentTimeMs());
    }

    document_->markDirty();
    hostOut = host;
    return true;
}

bool Engine::editableCompositionCancel(dom::Element*& hostOut) {
    return editableCompositionCommit("", hostOut, /*cancel=*/true);
}

void Engine::dispatchCompositionEvent(dom::Element* el, const char* type,
                                      const std::string& data) {
    if (!el) return;
    dom::CompositionEvent evt(type, true,
                              std::strcmp(type, "compositionstart") == 0);
    evt.setData(data);
    evt.setIsTrusted(true);
    dispatchEvent(el, evt);
}

void Engine::updateTextInputArea() {
    if (!window_ || !document_) return;
    auto* activeEl = document_->activeElement();
    float x = 0, y = 0, w = 0, h = 0;
    bool have = false;
    if (auto* input = getElInput(activeEl);
        input && input->isFocused() && input->isTextType(activeEl)) {
        have = input->caretRect(x, y, w, h);
    } else if (auto* ta = getElTextarea(activeEl); ta && ta->isFocused()) {
        have = ta->caretRect(x, y, w, h);
    } else if (textMetrics_) {
        auto* sel = document_->selection();
        auto* range = (sel && sel->rangeCount() > 0) ? sel->getRangeAt(0)
                                                     : nullptr;
        auto* node = range ? range->startContainer() : nullptr;
        if (node && document_->ownsNode(node) && inEditableHost(node) &&
            node->nodeType() == dom::NodeType::Text) {
            auto* tn = static_cast<dom::TextNode*>(node);
            float cx = 0, cy = 0, chh = 0;
            if (layout::getCaretRect(document_.get(), tn, range->startOffset(),
                                     *textMetrics_, cx, cy, chh)) {
                dom::Element* ctxEl = nullptr;
                for (dom::Node* n = tn; n; n = n->parentNode()) {
                    if (n->nodeType() == dom::NodeType::Element) {
                        ctxEl = static_cast<dom::Element*>(n);
                        break;
                    }
                }
                auto pr = ctxEl ? dom::projectRectThroughAncestors(ctxEl, cx, cy,
                                                                   1.0f, chh)
                                : dom::AbsoluteRect{cx, cy, 1.0f, chh};
                x = pr.x;
                y = pr.y - scrollY_;
                w = pr.width;
                h = pr.height;
                have = true;
            }
        }
    }
    if (!have) return;
    SDL_Rect rect;
    rect.x = static_cast<int>(std::lround(x));
    rect.y = static_cast<int>(std::lround(y + static_cast<float>(contentTop())));
    rect.w = static_cast<int>(std::lround(std::max(1.0f, w)));
    rect.h = static_cast<int>(std::lround(std::max(1.0f, h)));
    SDL_SetTextInputArea(window_->getSDLWindow(), &rect, 0);
}

void Engine::handleTextEditing(const std::string& text, int start,
                               int /*length*/) {
    if (!document_) return;
    if (overlayMgr_.hasActive()) return;

    auto* activeEl = document_->activeElement();
    auto* input = getElInput(activeEl);
    auto* ta = getElTextarea(activeEl);
    const bool inputOk = input && input->isFocused();
    const bool taOk = !inputOk && ta && ta->isFocused();
    if (!inputOk && !taOk) {
        if (text.empty()) {
            if (!editComp_.active) return;
            dom::Element* host = nullptr;
            if (!editableCompositionCancel(host)) return;
            dispatchCompositionEvent(host, "compositionupdate", "");
            dispatchInputEvent(host, "", "insertCompositionText", true);
            dispatchCompositionEvent(host, "compositionend", "");
        } else {
            bool wasComposing = false;
            std::string replacedSel;
            dom::Element* host = nullptr;
            if (!editableCompositionUpdate(text, start, wasComposing,
                                           replacedSel, host)) return;
            if (!wasComposing)
                dispatchCompositionEvent(host, "compositionstart", replacedSel);
            dispatchCompositionEvent(host, "compositionupdate", text);
            dispatchInputEvent(host, text, "insertCompositionText", true);
        }
        markAppBaseDirty();
        uiDirty_ = true;
        updateTextInputArea();
        return;
    }
    const bool wasComposing = inputOk ? input->isComposing() : ta->isComposing();

    if (text.empty()) {
        if (!wasComposing) return;
        layout::KeyHandleResult r = inputOk ? input->compositionCancel(activeEl)
                                            : ta->compositionCancel(activeEl);
        if (!r.handled) return;
        dispatchCompositionEvent(activeEl, "compositionupdate", "");
        dispatchInputEvent(activeEl, "", "insertCompositionText", true);
        dispatchCompositionEvent(activeEl, "compositionend", "");
        markAppBaseDirty();
        uiDirty_ = true;
        updateTextInputArea();
        return;
    }

    std::string replacedSel;
    if (!wasComposing)
        replacedSel = inputOk ? input->selectedText() : ta->selectedText();

    layout::KeyHandleResult r =
        inputOk ? input->compositionUpdate(activeEl, text, start)
                : ta->compositionUpdate(activeEl, text, start);
    if (!r.handled) return;

    if (!wasComposing)
        dispatchCompositionEvent(activeEl, "compositionstart", replacedSel);
    dispatchCompositionEvent(activeEl, "compositionupdate", text);
    dispatchInputEvent(activeEl, text, "insertCompositionText", true);
    markAppBaseDirty();
    uiDirty_ = true;
    updateTextInputArea();
}

void Engine::commitActiveComposition() {
    if (!document_) return;
    auto* activeEl = document_->activeElement();
    layout::KeyHandleResult r;
    std::string data;
    if (auto* input = getElInput(activeEl);
        input && input->isFocused() && input->isComposing()) {
        data = input->compositionText();
        r = input->compositionCommit(activeEl, data);
    } else if (auto* ta = getElTextarea(activeEl);
               ta && ta->isFocused() && ta->isComposing()) {
        data = ta->compositionText();
        r = ta->compositionCommit(activeEl, data);
    } else if (editComp_.active) {
        data = editComp_.preedit;
        dom::Element* host = nullptr;
        if (!editableCompositionCommit(data, host)) return;
        dispatchCompositionEvent(host, "compositionupdate", data);
        dispatchInputEvent(host, data, "insertCompositionText", true);
        dispatchCompositionEvent(host, "compositionend", data);
        markAppBaseDirty();
        uiDirty_ = true;
        updateTextInputArea();
        return;
    } else {
        return;
    }
    if (!r.handled) return;
    dispatchCompositionEvent(activeEl, "compositionupdate", data);
    dispatchInputEvent(activeEl, data, "insertCompositionText", true);
    dispatchCompositionEvent(activeEl, "compositionend", data);
    markAppBaseDirty();
    uiDirty_ = true;
    updateTextInputArea();
}

void Engine::handleTextInput(const std::string& text) {
    if (!document_) return;
    if (isControlChar(text)) return;

    if (overlayMgr_.handleTextInput(text)) {
        uiDirty_ = true;
        return;
    }

    auto* activeEl = document_->activeElement();
    layout::KeyHandleResult result;

    {
        layout::KeyHandleResult commit;
        if (auto* textarea = getElTextarea(activeEl);
            textarea && textarea->isFocused() && textarea->isComposing()) {
            commit = textarea->compositionCommit(activeEl, text);
        } else if (auto* input = getElInput(activeEl);
                   input && input->isFocused() && input->isComposing()) {
            commit = input->compositionCommit(activeEl, text);
        }
        if (commit.handled) {
            dispatchCompositionEvent(activeEl, "compositionupdate", text);
            dispatchInputEvent(activeEl, text, "insertCompositionText", true);
            dispatchCompositionEvent(activeEl, "compositionend", text);
            markAppBaseDirty();
            uiDirty_ = true;
            updateTextInputArea();
            return;
        }
        if (editComp_.active) {
            dom::Element* host = nullptr;
            if (editableCompositionCommit(text, host)) {
                dispatchCompositionEvent(host, "compositionupdate", text);
                dispatchInputEvent(host, text, "insertCompositionText", true);
                dispatchCompositionEvent(host, "compositionend", text);
                markAppBaseDirty();
                uiDirty_ = true;
                updateTextInputArea();
                return;
            }
        }
    }

    if (auto* textarea = getElTextarea(activeEl); textarea && textarea->isFocused()) {
        result = textarea->handleTextInput(activeEl, text);
    } else if (auto* input = getElInput(activeEl); input && input->isFocused()) {
        result = input->handleTextInput(activeEl, text);
    }

    if (result.handled) {
        applyKeyResult(activeEl, result);
        return;
    }

    editInsertTextAtSelection(text);
}

void Engine::simulatePaste(const std::string& text) {
    if (!document_) return;

    auto* activeEl = document_->activeElement();
    dom::Element* target = activeEl ? activeEl : document_->body();

    dom::ClipboardEvent pasteEvt("paste", true, true);
    pasteEvt.setClipboardText(text);
    if (!text.empty()) {
        pasteEvt.addItem({"text/plain", {}, text});
    }
    pasteEvt.setIsTrusted(true);
    dispatchEvent(target, pasteEvt);

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
            return;
        }
    }

    if (pasteEvt.defaultPrevented() || text.empty()) return;
    auto* sel = document_->selection();
    if (!sel || sel->rangeCount() == 0) return;
    auto* fn = sel->focusNode();
    auto* host = fn ? editableHostOf(fn) : nullptr;
    if (!host) return;
    runEditableMutation(document_.get(), fn,
        "insertFromPaste", text,
        [&] {
            EditUndoScope undo(&editUndo_, document_.get(), host, nullptr,
                               DomUndoStack::Kind::Discrete);
            selectionInsertText(document_.get(), text);
            undo.commit();
        });
    uiDirty_ = true;
}

std::string Engine::simulateCopy() {
    if (!document_) return "";

    auto* activeEl = document_->activeElement();
    dom::Element* target = activeEl ? activeEl : document_->body();

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
        auto* sel = document_->selection();
        if (sel && sel->rangeCount() > 0 && !sel->isCollapsed()) {
            auto* fn = sel->focusNode();
            if (fn && editableHostOf(fn)) text = sel->toString();
        }
    }

    dom::ClipboardEvent clipEvt("copy", true, true);
    clipEvt.setClipboardText(text);
    clipEvt.setIsTrusted(true);
    dispatchEvent(target, clipEvt);

    return text;
}

std::string Engine::simulateCut(
        const std::function<bool(const std::string&)>& commit) {
    if (!document_) return "";

    auto* activeEl = document_->activeElement();
    dom::Element* target = activeEl ? activeEl : document_->body();

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
    auto* sel = document_->selection();
    dom::Element* ceHost = nullptr;
    dom::Node* ceFocus = nullptr;
    if (!fromFormField && sel && sel->rangeCount() > 0 && !sel->isCollapsed()) {
        ceFocus = sel->focusNode();
        ceHost = ceFocus ? editableHostOf(ceFocus) : nullptr;
        if (ceHost) text = sel->toString();
    }

    dom::ClipboardEvent clipEvt("cut", true, true);
    clipEvt.setClipboardText(text);
    clipEvt.setIsTrusted(true);
    dispatchEvent(target, clipEvt);

    if (commit && !text.empty() && !commit(text)) return std::string();

    if (!clipEvt.defaultPrevented() && !text.empty() && activeEl) {
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
    }

    if (!clipEvt.defaultPrevented() && !text.empty() && ceHost) {
        runEditableMutation(document_.get(), ceFocus,
            "deleteByCut", "",
            [&] {
                EditUndoScope undo(&editUndo_, document_.get(), ceHost, nullptr,
                                   DomUndoStack::Kind::Discrete);
                auto* range = sel->getRangeAt(0);
                if (!range) return;
                dom::Node* after = nullptr; int afterOff = 0;
                deleteRangeContents(document_.get(), *range, after, afterOff);
                if (after) sel->collapse(after, afterOff);
                undo.commit();
            });
        uiDirty_ = true;
    }

    return text;
}

} // namespace bro::engine
