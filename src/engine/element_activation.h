#pragma once

// Activation behaviour — what a click DOES, once the click event itself has
// been dispatched and not cancelled.
//
// A click arrives from a real mouse (replaced_elements_input.cpp), from the
// keyboard (input_keyboard.cpp), from `element.click()` and from
// `element.dispatchEvent(new MouseEvent('click'))`, which DOM §2.9 says runs
// activation too. They all go through dispatchActivatingClick:
// label forwarding, the anchor download, the radio/checkbox/file toggles,
// submitting and resetting forms and opening a <details>. (It does not
// focus: element.click() never moves focus on the web; only a label's
// forward focuses the control it clicks.)
//
// This is that behaviour, once, at the engine layer where the hit-test path
// already lives.

namespace bro::dom {
class Element;
class MouseEvent;
}  // namespace bro::dom

namespace bro::engine {

/// Dispatch the click `click` at `target` with HTML's activation (DOM 2.9):
/// the activation target is `target` or its nearest ancestor with activation
/// behaviour; a checkbox or radio takes its new checkedness before the click
/// is dispatched (legacy-pre-activation), gets it back if the click is
/// cancelled (legacy-canceled-activation), and otherwise its activation
/// behaviour runs: input and change for a checkbox or radio, submitting or
/// resetting the form, the file picker, a link's download or navigation, a
/// label's forward to its control, a <summary>'s disclosure.
void dispatchActivatingClick(dom::Element* target, dom::MouseEvent& click);

/// `element.click()`: an untrusted click with activation, none at all for a
/// disabled control.
void clickElement(dom::Element* el);

/// The click a key makes (Enter or Space on a button, Space on a checkbox):
/// trusted, with activation.
void keyboardClick(dom::Element* el);

/// When a key activates the focused element `el`, as Chromium has it: Enter
/// clicks a button or a link at keydown; Space clicks a button, a checkbox or
/// a radio at keyup (keydown arms it).
enum class KeyActivation { None, Now, OnKeyUp };
KeyActivation keyActivationFor(dom::Element* el, bool enter, bool space);

/// Enter in a form's text field (HTML 4.10.21.2, implicit submission): click
/// the form's default button, or submit a form with no submit button and at
/// most one field that blocks implicit submission. True when `field` is in a
/// form.
bool implicitSubmission(dom::Element* field);

/// `form.requestSubmit(submitter)`: run constraint validation over the form's
/// controls (unless the form has novalidate or the submitter formnovalidate),
/// firing `invalid` at each failing one, and fire a cancelable `submit` at
/// the form only if every control passed.
void requestFormSubmit(dom::Element* form, dom::Element* submitter);

/// `form.reset()`: a cancelable `reset` event, then the controls' defaults.
void resetForm(dom::Element* form);

/// `form.checkValidity()` / `reportValidity()`: true when every control the
/// form owns is valid; fires `invalid` at each one that is not.
bool checkFormValidity(dom::Element* form);

}  // namespace bro::engine
