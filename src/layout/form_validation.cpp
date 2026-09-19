#include "layout/form_validation.h"

#include "dom/document.h"
#include "dom/element.h"
#include "layout/el_select.h"
#include "layout/form_control.h"

#include <cmath>
#include <cstdlib>

namespace bro::layout {

namespace {

bool tagIs(const dom::Element* el, const char* lower, const char* upper) {
    if (!el) return false;
    const std::string& t = el->tagName();
    return t == lower || t == upper;
}

std::string inputTypeOf(dom::Element* el) {
    if (!tagIs(el, "input", "INPUT")) return std::string();
    return el->getAttribute("type");
}

// utf-16 code-unit count, which is what `value.length` reports and therefore
// what minlength / maxlength count. The byte length std::string reports is the
// same only for ASCII, and every accented character made a field look longer
// than the user typed.
size_t utf16Units(const std::string& s) {
    size_t units = 0;
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80)        { i += 1; units += 1; }
        else if (c < 0xE0)   { i += 2; units += 1; }
        else if (c < 0xF0)   { i += 3; units += 1; }
        else                 { i += 4; units += 2; }  // astral: a surrogate pair
    }
    return units;
}

bool parseInt(const std::string& s, int& out) {
    try { size_t n = 0; int v = std::stoi(s, &n); if (n == 0) return false; out = v; return true; }
    catch (...) { return false; }
}

bool parseDouble(const std::string& s, double& out) {
    try { size_t n = 0; double v = std::stod(s, &n); if (n != s.size()) return false; out = v; return true; }
    catch (...) { return false; }
}

// Any radio in this control's name group — scoped to the owning form, or the
// document when there is none — currently checked.
bool anyRadioCheckedInGroup(dom::Element* el) {
    const std::string& name = el->getAttribute("name");
    dom::Element* root = formOwnerOf(el);
    if (!root && el->document()) root = el->document()->documentElement();
    if (!root) return el->hasAttribute("checked");
    bool found = false;
    std::function<void(dom::Element*)> walk = [&](dom::Element* e) {
        if (!e || found) return;
        if (tagIs(e, "input", "INPUT") && e->getAttribute("type") == "radio" &&
            e->getAttribute("name") == name && e->hasAttribute("checked")) {
            found = true;
            return;
        }
        for (auto* c : e->children()) walk(c);
    };
    walk(root);
    return found;
}

PatternTester& defaultTester() {
    static PatternTester t;
    return t;
}

}  // namespace

void setDefaultPatternTester(PatternTester tester) {
    defaultTester() = std::move(tester);
}

bool willValidate(const dom::Element* el) {
    if (!el) return false;
    const bool isInput = tagIs(el, "input", "INPUT");
    const bool isTextarea = tagIs(el, "textarea", "TEXTAREA");
    const bool isSelect = tagIs(el, "select", "SELECT");
    if (!isInput && !isTextarea && !isSelect) return false;
    if (el->hasAttribute("disabled") || el->hasAttribute("readonly")) return false;
    if (isInput) {
        const std::string& t = el->getAttribute("type");
        if (t == "hidden" || t == "button" || t == "submit" ||
            t == "reset"  || t == "image") return false;
    }
    return true;
}

std::string controlValue(dom::Element* el) {
    if (!el) return std::string();
    // <textarea>'s live value is its text content until something edits it;
    // formValue() already knows that rule, and the <select> rule, and the
    // pre-layout fallbacks for both.
    return formValue(el);
}

ValidityReport computeValidity(dom::Element* el, const PatternTester& testPattern) {
    ValidityReport r;
    if (!el) return r;
    if (!el->customValidity().empty()) r.customError = true;
    if (!willValidate(el)) return r;

    const std::string type = inputTypeOf(el);
    const std::string value = controlValue(el);
    const bool empty = value.empty();

    // ---- valueMissing -----------------------------------------------------
    if (el->hasAttribute("required")) {
        if (type == "checkbox") {
            if (!el->hasAttribute("checked")) r.valueMissing = true;
        } else if (type == "radio") {
            // A required radio is satisfied by ANY member of its group being
            // checked, not by this one — which is why a group of three with
            // the third picked used to report the first two invalid.
            if (!anyRadioCheckedInGroup(el)) r.valueMissing = true;
        } else if (empty) {
            r.valueMissing = true;
        }
    }

    // ---- typeMismatch / badInput (non-empty values only) ------------------
    if (!empty) {
        if (type == "email") {
            // Minimal: one '@' with something on each side, and a '.' after it.
            auto at = value.find('@');
            if (at == std::string::npos || at == 0 || at == value.size() - 1 ||
                value.find('.', at) == std::string::npos) {
                r.typeMismatch = true;
            }
        } else if (type == "url") {
            auto colon = value.find(':');
            if (colon == std::string::npos || colon < 2) r.typeMismatch = true;
        } else if (type == "number" || type == "range") {
            double n = 0;
            if (!parseDouble(value, n)) r.badInput = true;
        }
    }

    // ---- patternMismatch --------------------------------------------------
    const PatternTester& tester = testPattern ? testPattern : defaultTester();
    if (!empty && tester && el->hasAttribute("pattern")) {
        const std::string& pattern = el->getAttribute("pattern");
        if (!pattern.empty() && !tester(pattern, value)) r.patternMismatch = true;
    }

    // ---- minlength / maxlength -------------------------------------------
    const size_t len = utf16Units(value);
    if (!empty && el->hasAttribute("minlength")) {
        int min = 0;
        if (parseInt(el->getAttribute("minlength"), min) && min > 0 &&
            len < static_cast<size_t>(min)) {
            r.tooShort = true;
        }
    }
    if (el->hasAttribute("maxlength")) {
        int max = 0;
        if (parseInt(el->getAttribute("maxlength"), max) && max >= 0 &&
            len > static_cast<size_t>(max)) {
            r.tooLong = true;
        }
    }

    // ---- range underflow / overflow / step --------------------------------
    if (!r.badInput && !empty && (type == "number" || type == "range")) {
        double num = 0.0;
        parseDouble(value, num);
        double bound = 0.0;
        if (el->hasAttribute("min") && parseDouble(el->getAttribute("min"), bound) &&
            num < bound) {
            r.rangeUnderflow = true;
        }
        if (el->hasAttribute("max") && parseDouble(el->getAttribute("max"), bound) &&
            num > bound) {
            r.rangeOverflow = true;
        }
        if (el->hasAttribute("step")) {
            const std::string& s = el->getAttribute("step");
            double step = 0.0;
            if (s != "any" && parseDouble(s, step) && step > 0) {
                double base = 0.0;
                if (el->hasAttribute("min")) parseDouble(el->getAttribute("min"), base);
                const double off = num - base;
                // Fuzzy modulo: 0.3 - 0.1*3 is not 0 in binary floating point,
                // and a step of 0.1 is the commonest one there is.
                const double rem = off - std::round(off / step) * step;
                if (std::fabs(rem) > 1e-9) r.stepMismatch = true;
            }
        }
    }

    return r;
}

std::string defaultValidationMessage(const ValidityReport& r, dom::Element* el) {
    if (r.customError && el) return el->customValidity();
    if (r.valueMissing)    return "Please fill out this field.";
    if (r.typeMismatch)    return "Please enter a value of the correct type.";
    if (r.patternMismatch) return "Please match the requested format.";
    if (r.tooShort)        return "Please lengthen this text.";
    if (r.tooLong)         return "Please shorten this text.";
    if (r.rangeUnderflow)  return "Value is below the minimum.";
    if (r.rangeOverflow)   return "Value is above the maximum.";
    if (r.stepMismatch)    return "Please select a valid value.";
    if (r.badInput)        return "Please enter a valid value.";
    return std::string();
}

void collectFormElements(dom::Element* form, std::vector<dom::Element*>& out) {
    if (!form || !form->document()) return;
    if (!tagIs(form, "form", "FORM")) return;
    const std::string formId = form->getAttribute("id");

    auto isControl = [](const dom::Element* e) {
        return tagIs(e, "input", "INPUT") || tagIs(e, "select", "SELECT") ||
               tagIs(e, "textarea", "TEXTAREA") || tagIs(e, "button", "BUTTON") ||
               tagIs(e, "fieldset", "FIELDSET") || tagIs(e, "object", "OBJECT") ||
               tagIs(e, "output", "OUTPUT");
    };

    // The walk starts at the document element rather than at the form, because
    // `form="id"` associates a control that lives anywhere in the document.
    std::function<void(dom::Element*)> walk = [&](dom::Element* e) {
        if (!e) return;
        if (isControl(e)) {
            const std::string& ownerAttr = e->getAttribute("form");
            bool owned = false;
            if (!ownerAttr.empty()) {
                owned = (ownerAttr == formId && !formId.empty());
            } else {
                for (auto* p = e->parentElement(); p; p = p->parentElement()) {
                    if (p == form) { owned = true; break; }
                    if (tagIs(p, "form", "FORM")) break;   // a nested form owns it
                }
            }
            if (owned) out.push_back(e);
        }
        for (auto* c : e->children()) walk(c);
    };
    if (auto* root = form->document()->documentElement()) walk(root);
}

}  // namespace bro::layout
