#pragma once

// The IDL properties of a form control — `value`, `selectedIndex`, `options` —
// as engine operations rather than as binding code.
//
// None of these is a plain attribute read. `input.value` lives in the value
// attribute but a programmatic write also has to clear the control's undo
// history and re-arm its change event; `select.value` means "the value of the
// selected option", where the selection may live in a laid-out ElSelect or, if
// no layout pass has touched the element yet, only in the `selected`
// attributes; `textarea.value` starts as textContent and becomes the value
// attribute the moment anything edits it; `select.selectedIndex` has the same
// laid-out/not-yet-laid-out split, with a pending slot on the element bridging
// the two.
//
// WHY IT IS HERE AND NOT IN A BINDING FILE. It was written inside
// src/js/element_bindings.cpp, reachable from the QuickJS realm alone. The
// bronze host layer (src/bronze_host) binds the same DOM for compiled apps, and
// three.js's editor is built on a widget library that is nothing but these
// properties — every row of its sidebar is an <input> or a <select> read and
// written through them. A second implementation over there would be a second
// set of rules about where a <select>'s selection lives, and they would drift
// on the first bug fixed in one of them.
//
// What is NOT here: events. Setting a value from script fires neither `input`
// nor `change` (HTML reserves both for user interaction), so these functions
// arm rather than report, and the binding layers do not dispatch anything after
// calling them.

#include "dom/element.h"

#include <string>
#include <string_view>
#include <vector>

namespace bro::layout {

// Does this tag reflect a `value` IDL property at all? A <div>'s `.value` is an
// ordinary expando and must stay one, so the binding layers ask first and fall
// back to their own object storage when the answer is no.
bool reflectsValue(const dom::Element* el);

// The control's current value, by whichever rule its tag follows. "" for a tag
// that has no value.
std::string formValue(dom::Element* el);

// Write it, by the same rules — including the two side effects a script write
// owes: the undo history is dropped (the user cannot Ctrl+Z across a rewrite)
// and the change baseline is re-armed (see value_change.h).
void setFormValue(dom::Element* el, std::string_view value);

// A <select>'s <option> descendants in document order, descending through
// <optgroup>. Empty for anything else.
std::vector<dom::Element*> selectOptions(dom::Element* el);

// The selected index, from the laid-out control when there is one and from the
// DOM (pending assignment, then `selected` attributes, then "first option")
// when there is not. -1 for a <select> with no options, and for anything that
// is not a <select>.
int selectedIndex(dom::Element* el);
void setSelectedIndex(dom::Element* el, int index);

// element.tabIndex. Not a plain attribute read either: with no tabindex
// attribute the answer is 0 for the elements HTML makes focusable on their own
// — a link, a button, a form control — and -1 for everything else. A focus-ring
// walker tests exactly that difference, so answering a flat 0 or a flat -1
// makes every element either focusable or none of them.
int tabIndex(const dom::Element* el);
void setTabIndex(dom::Element* el, int value);

}  // namespace bro::layout
