// <input list=...> suggestions: the <datalist> options matching what the user
// typed, shown by DatalistOverlay under the field.
//
// The list opens on an edit by the user (typing, deleting, pasting: what
// applyKeyResult reports as an `input`) and on Down or Alt+Down in the field,
// never on a script's value write. Each later edit refreshes it in place; it
// closes when nothing matches, on Escape, Tab, a press outside it, and when
// the field loses the focus or leaves the document (syncDatalist, once a
// frame). Taking a row replaces the field's text as one undoable edit and
// fires `input` (inputType "insertReplacementText"), as browsers do; `change`
// follows on blur like any other edit.

#include "engine/engine.h"
#include "engine/datalist_overlay.h"
#include "engine/replaced_elements.h"
#include "dom/document.h"
#include "dom/element.h"
#include "layout/el_input.h"
#include "layout/form_control.h"
#include "layout/key_handle_result.h"
#include "render/recording_renderer.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace bro::engine {

namespace {

std::string lowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// The rows for the text typed: options whose value or label holds it, case
// aside. An explicit open (Down) on an empty field shows them all; an edit
// that leaves the field empty, or leaves only the option it already spells,
// shows none.
std::vector<DatalistOverlay::Option> matches(dom::Element* list, const std::string& typed,
                                             bool explicitOpen) {
    std::vector<DatalistOverlay::Option> out;
    if (typed.empty() && !explicitOpen) return out;
    const std::string needle = lowerAscii(typed);
    for (auto& o : layout::datalistOptions(list)) {
        if (!needle.empty() &&
            lowerAscii(o.value).find(needle) == std::string::npos &&
            lowerAscii(o.label).find(needle) == std::string::npos) {
            continue;
        }
        out.push_back({std::move(o.value), std::move(o.label)});
    }
    if (!explicitOpen && out.size() == 1 && out[0].value == typed) out.clear();
    return out;
}

DatalistOverlay* activeDatalist(OverlayManager& mgr) {
    return dynamic_cast<DatalistOverlay*>(mgr.active());
}

// The field takes suggestions: a text-like input, enabled, writable.
bool takesSuggestions(dom::Element* el, layout::ElInput* input) {
    if (!input || !input->isFocused() || !input->isTextType(el)) return false;
    using T = layout::ElInput::InputType;
    if (input->inputType(el) == T::Password) return false;
    return !el->hasAttribute("disabled") && !el->hasAttribute("readonly");
}

}  // namespace

void Engine::updateDatalist(dom::Element* el, bool explicitOpen) {
    if (datalistPicking_ || !document_ || !el) return;
    DatalistOverlay* open = activeDatalist(*overlayMgr_);
    if (open && open->dismissRequested()) return;

    auto* input = getElInput(el);
    dom::Element* list = layout::inputList(el);
    if (!list || !takesSuggestions(el, input) || el != document_->activeElement()) {
        if (open && open->owner() == el) overlayMgr_->close();
        return;
    }

    auto rows = matches(list, layout::formValue(el), explicitOpen);
    if (rows.empty()) {
        if (open && open->owner() == el) {
            overlayMgr_->close();
            markAppBaseDirty();
        }
        return;
    }
    if (open && open->owner() == el) {
        open->setOptions(std::move(rows));
        markAppBaseDirty();
        return;
    }
    // Another overlay (a <select>'s, a colour picker) stays: suggestions do
    // not take its place.
    if (overlayMgr_->hasActive()) return;

    std::string family = "Arial";
    float fontSize = 16.0f;
    {
        auto& style = el->computedStyle();
        auto it = style.find("font-family");
        if (it != style.end() && !it->second.empty()) family = it->second;
        auto sit = style.find("font-size");
        if (sit != style.end()) {
            char* end = nullptr;
            float v = std::strtof(sit->second.c_str(), &end);
            if (end != sit->second.c_str() && v > 0) fontSize = v;
        }
    }

    dom::Document* doc = document_.get();
    dom::ElementHandle anchorHandle(doc, el);
    auto anchor = [anchorHandle](float& x, float& y, float& w, float& h) {
        dom::Element* e = anchorHandle.get();
        auto* in = e ? getElInput(e) : nullptr;
        if (!in) return false;
        auto dp = in->lastDrawPos();
        if (dp.w <= 0 || dp.h <= 0) return false;
        x = dp.x; y = dp.y; w = dp.w; h = dp.h;
        return true;
    };

    dom::ElementHandle pickHandle(doc, el);
    auto onPick = [this, pickHandle](const std::string& value) {
        dom::Element* e = pickHandle.get();
        auto* in = e ? getElInput(e) : nullptr;
        if (!in || !document_ || e != document_->activeElement()) return;
        // The whole text replaced as one edit: undoable, and the caret after it.
        in->selectAll();
        layout::KeyHandleResult r = in->pasteText(e, value);
        if (!r.handled) return;
        r.inputType = "insertReplacementText";
        r.inputData = value;
        datalistPicking_ = true;
        applyKeyResult(e, r);
        datalistPicking_ = false;
    };

    auto overlay = std::make_unique<DatalistOverlay>(
        el, std::move(anchor), static_cast<float>(contentHeight()), std::move(rows),
        std::move(family), fontSize, std::move(onPick));
    overlayMgr_->open(std::move(overlay), OverlayContext::App, recordingRenderer_.get());
    markAppBaseDirty();
}

void Engine::syncDatalist() {
    DatalistOverlay* open = activeDatalist(*overlayMgr_);
    if (!open) return;
    dom::Element* active = document_ ? document_->activeElement() : nullptr;
    auto* input = active ? getElInput(active) : nullptr;
    // The owner is compared as an address only, and only against the element
    // that is focused now, so a freed owner is never touched.
    if (!active || open->owner() != active || !input || !input->isFocused() ||
        !layout::inputList(active)) {
        overlayMgr_->close();
        markAppBaseDirty();
    }
}

}  // namespace bro::engine
