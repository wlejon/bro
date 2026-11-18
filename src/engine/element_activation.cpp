#include "engine/element_activation.h"

#include "bronze_host/host_anchor_download.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"
#include "dom/node_handle.h"
#include "layout/el_select.h"
#include "layout/form_control.h"
#include "layout/form_validation.h"
#include "platform/dialogs.h"

#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace bro::bronze_host {
// element.focus()'s steps (declared in host_events.h, which is too heavy to
// pull into the engine layer for one function).
void hostFocusElement(dom::Element* el);
// A link's activation: navigate to its href (host_location.cpp).
void hostFollowHyperlink(dom::Element* anchor);
}  // namespace bro::bronze_host

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

// The <details> whose disclosure handle `el` is: the first <summary> child of
// a <details>, else null.
dom::Element* detailsOfSummary(dom::Element* el) {
    if (!tagIs(el, "summary", "SUMMARY")) return nullptr;
    dom::Element* parent = el->parentElement();
    if (!parent || !tagIs(parent, "details", "DETAILS")) return nullptr;
    for (auto* c : parent->children()) {
        if (tagIs(c, "summary", "SUMMARY")) return c == el ? parent : nullptr;
    }
    return nullptr;
}

void toggleDetails(dom::Element* details) {
    if (details->hasAttribute("open")) details->removeAttribute("open");
    else                               details->setAttribute("open", "");
    dom::Event toggleEvt("toggle", false, false);
    toggleEvt.setIsTrusted(true);
    dom::dispatchDomEvent(details, toggleEvt);
}

std::string inputTypeOf(const dom::Element* el) {
    return tagIs(el, "input", "INPUT") ? lowered(el->getAttribute("type")) : std::string();
}

// Whether `el` has activation behaviour (HTML 6.2.1 / DOM 2.9): the click
// that reaches it does something once dispatched.
bool hasActivationBehavior(dom::Element* el) {
    if (tagIs(el, "a", "A") || tagIs(el, "area", "AREA")) return el->hasAttribute("href");
    if (tagIs(el, "button", "BUTTON") || tagIs(el, "label", "LABEL")) return true;
    if (tagIs(el, "input", "INPUT")) {
        const std::string t = inputTypeOf(el);
        return t == "checkbox" || t == "radio" || t == "submit" || t == "reset" || t == "image" ||
               t == "file" || t == "button" || t == "color";
    }
    return detailsOfSummary(el) != nullptr;
}

// What a checkbox or radio's legacy-pre-activation behaviour changed, for
// its legacy-canceled-activation behaviour to put back.
struct PreActivation {
    bool checkable = false;
    bool wasChecked = false;
    dom::ElementHandle previousRadio;
};

PreActivation legacyPreActivation(dom::Element* el) {
    PreActivation pre;
    const std::string t = inputTypeOf(el);
    if (t != "checkbox" && t != "radio") return pre;
    pre.checkable = true;
    pre.wasChecked = el->hasAttribute("checked");
    if (t == "checkbox") {
        if (pre.wasChecked) el->removeAttribute("checked");
        else                el->setAttribute("checked", "");
    } else if (!pre.wasChecked) {
        pre.previousRadio.assign(el->document(), layout::checkedRadioInGroup(el));
        layout::clearRadioGroup(el);
        el->setAttribute("checked", "");
    }
    return pre;
}

void legacyCanceledActivation(dom::Element* el, const PreActivation& pre) {
    if (inputTypeOf(el) == "checkbox") {
        if (pre.wasChecked) el->setAttribute("checked", "");
        else                el->removeAttribute("checked");
    } else if (!pre.wasChecked) {
        el->removeAttribute("checked");
        // Back to the member that held the check; none if it has since gone.
        if (dom::Element* prev = pre.previousRadio.get()) prev->setAttribute("checked", "");
    }
}

// A <label>'s activation behaviour: focus its labeled control and click it,
// unless the click came from the control itself or from other interactive
// content inside the label (a button or a link beside a checkbox must not
// tick it). The forwarded click carries the original's coordinates and
// trust.
void labelActivation(dom::Element* label, const dom::MouseEvent& click) {
    dom::Element* control = layout::findLabeledControl(label);
    if (!control || activationDisabled(control)) return;
    auto* target = static_cast<dom::Element*>(click.target());
    for (auto* el = target; el && el != label; el = el->parentElement()) {
        if (el == control || layout::isInteractiveContent(el)) return;
    }
    // Focused through element.focus()'s steps, so the engine knows (Space
    // toggles a checkbox afterwards), as a browser's label does for a real
    // click and for label.click() alike.
    bronze_host::hostFocusElement(control);
    dom::MouseEvent forwarded("click");
    forwarded.setClientX(click.clientX()); forwarded.setClientY(click.clientY());
    forwarded.setScreenX(click.screenX()); forwarded.setScreenY(click.screenY());
    forwarded.setPageX(click.pageX());     forwarded.setPageY(click.pageY());
    forwarded.setCtrlKey(click.ctrlKey()); forwarded.setShiftKey(click.shiftKey());
    forwarded.setAltKey(click.altKey());   forwarded.setMetaKey(click.metaKey());
    forwarded.setDetail(click.detail());
    forwarded.setIsTrusted(click.isTrusted());
    dispatchActivatingClick(control, forwarded);
}

// The activation behaviour of `el`, its click dispatched and not cancelled.
void runActivationBehavior(dom::Element* el, const dom::MouseEvent& click) {
    const std::string t = inputTypeOf(el);
    if (t == "checkbox" || t == "radio") {
        dom::InputEvent inputEvt("input");
        inputEvt.setIsTrusted(true);
        dom::dispatchDomEvent(el, inputEvt);
        dom::Event changeEvt("change", true, false);
        changeEvt.setIsTrusted(true);
        dom::dispatchDomEvent(el, changeEvt);
        return;
    }
    if (activationDisabled(el)) return;
    if (tagIs(el, "button", "BUTTON") || t == "submit" || t == "reset" || t == "image") {
        std::string type = lowered(el->getAttribute("type"));
        if (tagIs(el, "button", "BUTTON") && type != "reset" && type != "button") type = "submit";
        if (type == "image") type = "submit";
        if (type == "submit") {
            if (dom::Element* owner = formOwnerFor(el)) requestFormSubmit(owner, el);
        } else if (type == "reset") {
            if (dom::Element* owner = formOwnerFor(el)) resetForm(owner);
        }
        return;
    }
    if (runFilePickerActivation(el)) return;
    if (tagIs(el, "a", "A") || tagIs(el, "area", "AREA")) {
        // <a download>: save the link's bytes. Building a Blob, minting an
        // object URL and calling link.click() is how every "Export" button on
        // the web works.
        if (bronze_host::runAnchorDownload(el)) return;
        bronze_host::hostFollowHyperlink(el);
        return;
    }
    if (tagIs(el, "label", "LABEL")) {
        labelActivation(el, click);
        return;
    }
    if (dom::Element* details = detailsOfSummary(el)) toggleDetails(details);
}

}  // namespace

void dispatchActivatingClick(dom::Element* target, dom::MouseEvent& click) {
    if (!target) return;
    // The activation target: the target, else the nearest ancestor on the
    // event's path with activation behaviour (a <span> in a link or a label).
    dom::Element* activation = nullptr;
    for (auto* el = target; el; el = el->parentElement()) {
        if (hasActivationBehavior(el)) { activation = el; break; }
    }
    dom::ElementHandle activationHandle(target->document(), activation);
    PreActivation pre;
    if (activation) pre = legacyPreActivation(activation);
    dom::dispatchDomEvent(target, click);
    activation = activationHandle.get();
    if (!activation) return;
    if (click.defaultPrevented()) {
        if (pre.checkable) legacyCanceledActivation(activation, pre);
        return;
    }
    // A radio that was already checked changes nothing: no input, no change.
    if (pre.checkable && inputTypeOf(activation) == "radio" && pre.wasChecked) return;
    runActivationBehavior(activation, click);
}

void clickElement(dom::Element* el) {
    if (!el) return;
    // A disabled control gets no click at all, as in a browser (and as the
    // hit-tested path's isInDisabledControl gate has it). No focus change
    // either: element.click() never moves focus on the web.
    if (activationDisabled(el)) return;
    dom::MouseEvent click("click");
    dispatchActivatingClick(el, click);
}

void keyboardClick(dom::Element* el) {
    if (!el || activationDisabled(el)) return;
    dom::MouseEvent click("click");
    click.setIsTrusted(true);
    dispatchActivatingClick(el, click);
}

KeyActivation keyActivationFor(dom::Element* el, bool enter, bool space) {
    if (!el || (!enter && !space)) return KeyActivation::None;
    const std::string t = inputTypeOf(el);
    const bool buttonLike = tagIs(el, "button", "BUTTON") || t == "submit" || t == "reset" ||
                            t == "image" || t == "button" || t == "color" || t == "file" ||
                            detailsOfSummary(el) != nullptr;
    if (enter) {
        if (buttonLike) return KeyActivation::Now;
        if ((tagIs(el, "a", "A") || tagIs(el, "area", "AREA")) && el->hasAttribute("href"))
            return KeyActivation::Now;
        return KeyActivation::None;
    }
    if (buttonLike || t == "checkbox" || t == "radio") return KeyActivation::OnKeyUp;
    return KeyActivation::None;
}

bool implicitSubmission(dom::Element* field) {
    if (!field || !tagIs(field, "input", "INPUT")) return false;
    dom::Element* form = formOwnerFor(field);
    if (!form) return false;
    std::vector<dom::Element*> items;
    layout::collectFormElements(form, items);
    // The form's default button: its first submit button in tree order.
    for (dom::Element* c : items) {
        const std::string t = inputTypeOf(c);
        const bool submitButton = (tagIs(c, "button", "BUTTON") &&
                                   lowered(c->getAttribute("type")) != "reset" &&
                                   lowered(c->getAttribute("type")) != "button") ||
                                  t == "submit" || t == "image";
        if (!submitButton) continue;
        if (!activationDisabled(c)) keyboardClick(c);
        return true;
    }
    // No default button: submit when no more than one field blocks implicit
    // submission (HTML 4.10.21.2).
    int blocking = 0;
    for (dom::Element* c : items) {
        if (!tagIs(c, "input", "INPUT")) continue;
        const std::string t = inputTypeOf(c);
        if (t.empty() || t == "text" || t == "search" || t == "url" || t == "tel" || t == "email" ||
            t == "password" || t == "date" || t == "month" || t == "week" || t == "time" ||
            t == "datetime-local" || t == "number")
            ++blocking;
    }
    if (blocking <= 1) requestFormSubmit(form, nullptr);
    return true;
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
    const bool noValidate = form->hasAttribute("novalidate") ||
                            (submitter && submitter->hasAttribute("formnovalidate"));
    if (!noValidate && !checkFormValidity(form)) return;

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
