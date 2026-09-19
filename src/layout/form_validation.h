#pragma once

// HTML constraint validation as an engine operation.
//
// `input.validity`, `input.validationMessage`, `input.willValidate`,
// `input.checkValidity()`, `form.checkValidity()` and the validation step
// `form.requestSubmit()` runs are all ONE computation over the DOM, and none of
// it needs a JS realm. It used to live inline in the QuickJS element binding;
// the bronze port kept only three of the ten constraints (customError,
// required-empty, pattern) and dropped the form-wide walk entirely, so
// `form.checkValidity()` ran the ELEMENT check on the <form> — which has no
// value and no constraints — and answered true for every form ever written.
//
// The one piece that genuinely needs a realm is `pattern`: HTML defines it as
// an ECMAScript RegExp (named groups, lookbehind, \u{...}), which is the JS
// engine's dialect and not std::regex's. It arrives as a callback so the rest
// of the computation stays here.

#include <functional>
#include <string>
#include <vector>

namespace bro::dom { class Element; }

namespace bro::layout {

struct ValidityReport {
    bool valueMissing = false;
    bool typeMismatch = false;
    bool patternMismatch = false;
    bool tooShort = false;
    bool tooLong = false;
    bool rangeUnderflow = false;
    bool rangeOverflow = false;
    bool stepMismatch = false;
    bool badInput = false;
    bool customError = false;
    bool valid() const {
        return !(valueMissing || typeMismatch || patternMismatch || tooShort ||
                 tooLong || rangeUnderflow || rangeOverflow || stepMismatch ||
                 badInput || customError);
    }
};

/// Answers whether `value` matches `pattern` under the implicit `^(?:...)$`
/// anchoring HTML specifies. Supplied by the layer that has a RegExp engine;
/// an empty callback means the pattern constraint is skipped (which is also
/// what the spec does with a pattern that fails to compile).
using PatternTester =
    std::function<bool(const std::string& pattern, const std::string& value)>;

/// Install the process-wide tester. The bronze host does this once at startup,
/// so every caller of computeValidity() — engine, host, a compiled program's
/// form submit — gets the pattern constraint without having to carry a RegExp
/// engine of its own.
void setDefaultPatternTester(PatternTester tester);

/// HTML's "candidate for constraint validation": a form-associated control
/// that is not disabled, not readonly, and not one of the input types that
/// carries no value (hidden/button/submit/reset/image).
bool willValidate(const dom::Element* el);

/// The value constraint validation reads: textarea content, the selected
/// option's value, or the value attribute.
std::string controlValue(dom::Element* el);

/// Every constraint HTML defines, over one control. With no tester passed, the
/// one installed by setDefaultPatternTester() is used.
ValidityReport computeValidity(dom::Element* el, const PatternTester& testPattern = {});

/// The browser-visible message for a failing report; "" for a valid one.
std::string defaultValidationMessage(const ValidityReport& r, dom::Element* el);

/// The controls a <form> owns, in document order — descendants, plus anything
/// anywhere in the document whose `form` attribute names this form's id.
/// Empty for anything that is not a <form>.
void collectFormElements(dom::Element* form, std::vector<dom::Element*>& out);

}  // namespace bro::layout
