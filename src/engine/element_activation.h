#pragma once

// Activation behaviour — what a click DOES, once the click event itself has
// been dispatched and not cancelled.
//
// There are two ways a click arrives: the hit-tested one
// (replaced_elements_input.cpp, from a real mouse) and the programmatic one
// (`element.click()`, and `element.dispatchEvent(new MouseEvent('click'))`,
// which DOM §2.9 says runs activation too). They owe the same behaviour, and
// the QuickJS binding had a second copy of it inline. The bronze port
// transcribed only part of that copy — label forwarding, the anchor download,
// and the radio/checkbox/file toggles — so a programmatic click stopped
// submitting and resetting forms and stopped opening a <details>. (It does
// not focus: element.click() never moves focus on the web; only a label's
// forward focuses the control it clicks.)
//
// This is that behaviour, once, at the engine layer where the hit-test path
// already lives.

namespace bro::dom { class Element; }

namespace bro::engine {

/// `element.click()`: dispatch a click at `el`, then run its activation
/// behaviour and, if `el` sits inside a <label>, forward to the labeled
/// control exactly as a real click on the label's text does.
void clickElement(dom::Element* el);

/// The activation behaviour alone, with the click already dispatched and not
/// cancelled: form submit/reset, checkbox/radio toggle, the file
/// picker, the anchor download, the <details> disclosure. Safe to call for any
/// element — it does nothing for one with no activation behaviour.
void runActivationBehavior(dom::Element* el);

/// A click landed on `clickTarget`; if it is inside a <label> whose control is
/// elsewhere, activate that control. Returns true if it did. Interactive
/// content between the target and the label blocks the forward, so a button
/// beside a checkbox does not tick it.
bool forwardLabelActivation(dom::Element* clickTarget);

/// `form.requestSubmit(submitter)`: run constraint validation over the form's
/// controls, firing `invalid` at each failing one, and fire a cancelable
/// `submit` at the form only if every control passed.
void requestFormSubmit(dom::Element* form, dom::Element* submitter);

/// `form.reset()`: a cancelable `reset` event, then the controls' defaults.
void resetForm(dom::Element* form);

/// `form.checkValidity()` / `reportValidity()`: true when every control the
/// form owns is valid; fires `invalid` at each one that is not.
bool checkFormValidity(dom::Element* form);

}  // namespace bro::engine
