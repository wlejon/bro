#include "engine/element_activation.h"

#include "bronze_host/host_anchor_download.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"
#include "layout/el_select.h"
#include "layout/form_control.h"
#include "layout/form_validation.h"
#include "platform/dialogs.h"

#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace bro::engine {

namespace {

bool tagIs(const dom::Element* el, const char* lower, const char* upper) {
    if (!el) return false;
    const std::string& t = el->tagName();
    return t == lower || t == upper;
}

std::string lowered(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

dom::Element* formOwnerFor(dom::Element* el) {
    if (!el) return nullptr;
    const std::string& attrForm = el->getAttribute("form");
    if (!attrForm.empty() && el->document()) {
        dom::Element* o = el->document()->getElementById(attrForm);
        return (o && tagIs(o, "form", "FORM")) ? o : nullptr;
    }
    for (auto* p = el->parentElement(); p; p = p->parentElement()) {
        if (tagIs(p, "form", "FORM")) return p;
    }
    return nullptr;
}

// A disabled control has no activation behaviour, and neither does anything
// inside a disabled <fieldset>.
bool activationDisabled(dom::Element* el) {
    for (auto* p = el; p; p = p->parentElement()) {
        if (!p->hasAttribute("disabled")) continue;
        if (p == el || tagIs(p, "fieldset", "FIELDSET")) return true;
    }
    return false;
}

// <input type=file>: open the native picker and report the pick. Returns true
// when this element IS a file input (whether or not anything was picked), so
// the caller knows the activation was consumed.
bool runFilePickerActivation(dom::Element* el) {
    if (!tagIs(el, "input", "INPUT")) return false;
    if (lowered(el->getAttribute("type")) != "file") return false;
    if (el->hasAttribute("disabled")) return false;

    std::vector<std::string> picked =
        platform::Dialogs::pickFiles(el->getAttribute("accept"),
                                     el->hasAttribute("multiple"));
    if (picked.empty()) return true;   // cancelled: no events, per spec

    el->setSelectedFiles(picked);
    std::string first = std::filesystem::path(picked.front()).filename().string();
    el->setAttribute("value", "C:\\fakepath\\" + first);

    dom::InputEvent inputEvt("input");
    inputEvt.setIsTrusted(true);
    dom::dispatchDomEvent(el, inputEvt);
    dom::Event changeEvt("change", true, false);
    changeEvt.setIsTrusted(true);
    dom::dispatchDomEvent(el, changeEvt);
    return true;
}

// <summary> activation: toggle [open] on the parent <details>. The walk starts
// at `el` so a click on an icon inside the summary counts, but only the FIRST
// <summary> child of a <details> is the disclosure handle.
bool toggleDetailsFromSummary(dom::Element* el) {
    for (auto* s = el; s; s = s->parentElement()) {
        if (!tagIs(s, "summary", "SUMMARY")) continue;
        dom::Element* parent = s->parentElement();
        if (!parent || !tagIs(parent, "details", "DETAILS")) return false;
        dom::Element* firstSummary = nullptr;
        for (auto* c : parent->children()) {
            if (tagIs(c, "summary", "SUMMARY")) { firstSummary = c; break; }
        }
        if (firstSummary != s) return false;
        if (parent->hasAttribute("open")) parent->removeAttribute("open");
        else                              parent->setAttribute("open", "");
        dom::Event toggleEvt("toggle", false, false);
        toggleEvt.setIsTrusted(true);
        dom::dispatchDomEvent(parent, toggleEvt);
        return true;
    }
    return false;
}

}  // namespace

void runActivationBehavior(dom::Element* el) {
    if (!el) return;

    const bool isButton = tagIs(el, "button", "BUTTON");
    const bool isInput = tagIs(el, "input", "INPUT");
    const std::string inputType = isInput ? lowered(el->getAttribute("type")) : std::string();
    const bool isActionInput =
        isInput && (inputType == "submit" || inputType == "reset" || inputType == "image");

    if (isButton || isActionInput) {
        std::string btnType = lowered(el->getAttribute("type"));
        if (btnType.empty() && isButton) btnType = "submit";
        if (btnType == "image") btnType = "submit";   // image = implicit submit
        if (btnType == "submit") {
            if (dom::Element* owner = formOwnerFor(el)) requestFormSubmit(owner, el);
        } else if (btnType == "reset") {
            if (dom::Element* owner = formOwnerFor(el)) resetForm(owner);
        }
        return;
    }

    if (isInput && (inputType == "checkbox" || inputType == "radio")) {
        if (inputType == "checkbox") {
            if (el->hasAttribute("checked")) el->removeAttribute("checked");
            else                            el->setAttribute("checked", "");
        } else {
            // Radio: checking one unchecks the rest of its name group. An
            // already-checked radio does not fire again.
            if (el->hasAttribute("checked")) return;
            layout::clearRadioGroup(el);
            el->setAttribute("checked", "");
        }
        dom::Event changeEvt("change");
        dom::dispatchDomEvent(el, changeEvt);
        dom::InputEvent inputEvt("input");
        inputEvt.setIsTrusted(true);
        dom::dispatchDomEvent(el, inputEvt);
        return;
    }

    if (runFilePickerActivation(el)) return;

    // <a download>: save the link's bytes instead of navigating. Building a
    // Blob, minting an object URL and calling link.click() is how every
    // "Export" button on the web works, so this is the programmatic path's
    // most-used branch.
    if (bronze_host::runAnchorDownload(el)) return;

    toggleDetailsFromSummary(el);
}

bool forwardLabelActivation(dom::Element* clickTarget) {
    if (!clickTarget) return false;
    dom::Element* label = nullptr;
    for (auto* el = clickTarget; el; el = el->parentElement()) {
        if (tagIs(el, "label", "LABEL")) { label = el; break; }
        // Interactive content inside the label handles its own clicks: a
        // button or a link next to the checkbox must not tick it. The click
        // target itself only blocks when it is labelable — clicking the
        // label's own <span> must still forward.
        if (el != clickTarget || layout::isLabelable(el)) {
            if (layout::isLabelable(el) || tagIs(el, "a", "A")) return false;
        }
    }
    if (!label) return false;
    dom::Element* control = layout::findLabeledControl(label);
    if (!control || control == clickTarget) return false;
    // The click already went to the control (or something inside it), so the
    // control has already run its own activation. Forwarding here is what would
    // toggle a wrapped checkbox twice and leave it apparently dead.
    for (auto* el = clickTarget; el; el = el->parentElement()) {
        if (el == control) return false;
        if (el == label) break;
    }
    if (activationDisabled(control)) return false;
    clickElement(control);
    return true;
}

void clickElement(dom::Element* el) {
    if (!el) return;
    // A disabled control has no activation behaviour at all — not even the
    // click event. Matches the hit-tested path's isInDisabledControl gate.
    if (activationDisabled(el)) return;

    if (dom::Document* doc = el->document()) doc->setActiveElement(el);

    dom::MouseEvent clickEvt("click");
    dom::dispatchDomEvent(el, clickEvt);
    if (!clickEvt.defaultPrevented()) runActivationBehavior(el);

    // A programmatic label.click() forwards to its control, exactly as a real
    // click on the label's text does.
    forwardLabelActivation(el);
}

bool checkFormValidity(dom::Element* form) {
    std::vector<dom::Element*> items;
    layout::collectFormElements(form, items);
    bool allValid = true;
    for (dom::Element* c : items) {
        layout::ValidityReport r = layout::computeValidity(c);
        if (r.valid()) continue;
        allValid = false;
        dom::Event evt("invalid", false, true);
        evt.setIsTrusted(true);
        dom::dispatchDomEvent(c, evt);
    }
    return allValid;
}

void requestFormSubmit(dom::Element* form, dom::Element* submitter) {
    if (!form) return;
    if (!checkFormValidity(form)) return;

    dom::SubmitEvent evt("submit", true, true);
    evt.setIsTrusted(true);
    evt.setSubmitter(submitter);
    dom::dispatchDomEvent(form, evt);
    // Not cancelled: bro has no default navigation — the app owns it.
}

void resetForm(dom::Element* form) {
    if (!form) return;
    dom::Event resetEvt("reset", true, true);
    resetEvt.setIsTrusted(true);
    dom::dispatchDomEvent(form, resetEvt);
    if (resetEvt.defaultPrevented()) return;

    std::vector<dom::Element*> items;
    layout::collectFormElements(form, items);
    for (dom::Element* c : items) {
        if (tagIs(c, "select", "SELECT")) {
            // Back to the option carrying `selected`, or the first one.
            int idx = 0, found = -1;
            for (auto* child : c->children()) {
                if (!tagIs(child, "option", "OPTION")) continue;
                if (child->hasAttribute("selected") && found < 0) found = idx;
                ++idx;
            }
            layout::setSelectedIndex(c, found >= 0 ? found : 0);
        }
        // Inputs and textareas: this DOM stores the live value in the same
        // `value` attribute that carries the default, so there is no
        // defaultValue to go back to. Left alone rather than cleared, which
        // would be worse than doing nothing.
    }
}

}  // namespace bro::engine
