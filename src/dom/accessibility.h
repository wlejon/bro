#pragma once

// An element's accessible role and name, from the DOM alone: the one
// computation behind both the platform accessibility tree (src/a11y) and the
// agent view (engine/view_document.h, docs/agent-view.md).
//
// Roles are ARIA role names ("button", "textbox", "listitem", ...): an
// explicit role="" wins, else the element's implicit role; "" for an element
// with none (a generic wrapper). Names follow the accessible-name order a
// browser uses, simplified: aria-labelledby, aria-label, a <label> (for= or
// wrapping) for form controls, alt / title / placeholder, then the text
// content for roles named from their content.

#include <string>

namespace bro::dom {

class Document;
class Element;

/// The lower-case local tag name ("button", "input").
std::string lowerTagName(const Element* el);

/// The element's ARIA role, explicit or implicit; "" for none.
std::string accessibleRole(const Element* el);

/// Whether `role` is one a user operates (a button, a field, an option).
bool isInteractiveRole(const std::string& role);

/// Whether an element with `role` takes its name from its text content.
bool roleNamedFromContent(const std::string& role);

/// The element's accessible name, whitespace collapsed (not truncated).
/// `role` is accessibleRole(el), passed when the caller has it.
std::string accessibleName(const Element* el, Document* doc, const std::string& role);
std::string accessibleName(const Element* el, Document* doc);

/// Text with runs of whitespace collapsed to one space and the ends trimmed.
std::string collapseWhitespace(const std::string& text);

/// The text content of `el`'s rendered subtree (display:none and aria-hidden
/// parts left out), whitespace collapsed, stopping once it has `maxBytes`.
std::string renderedText(const Element* el, size_t maxBytes = 512);

}  // namespace bro::dom
