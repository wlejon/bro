// Form control IDL properties and DOM interaction helpers.

#include "layout/form_control.h"

#include "dom/document.h"
#include "layout/el_input.h"
#include "layout/el_select.h"
#include "layout/el_textarea.h"

#include <algorithm>
#include <cstdlib>

namespace bro::layout {
namespace {

bool tagIs(const dom::Element* el, std::string_view lower, std::string_view upper) {
    const std::string& t = el->tagName();
    return t == lower || t == upper;
}

// An option's value falls back to its text only when the value attribute is
// ABSENT — an explicit value="" stays "" (placeholder options depend on it).
std::string optionValue(dom::Element* o) {
    return o->hasAttribute("value") ? o->getAttribute("value") : o->textContent();
}

void collectOptions(dom::Element* el, std::vector<dom::Element*>& out) {
    for (auto* child : el->children()) {
        if (tagIs(child, "option", "OPTION")) out.push_back(child);
        else if (tagIs(child, "optgroup", "OPTGROUP")) collectOptions(child, out);
    }
}

}  // namespace

bool reflectsValue(const dom::Element* el) {
    if (!el) return false;
    static const char* const kTags[] = {
        "input", "textarea", "select", "option", "button", "progress",
        "meter", "output", "li", "data", "param",
    };
    const std::string& t = el->tagName();
    for (const char* tag : kTags) {
        std::string_view lower(tag);
        if (t.size() != lower.size()) continue;
        bool same = true;
        for (size_t i = 0; i < t.size(); ++i) {
            char a = t[i], b = lower[i];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (a != b) { same = false; break; }
        }
        if (same) return true;
    }
    return false;
}

std::string formValue(dom::Element* el) {
    if (!el || !reflectsValue(el)) return std::string();

    // For <select>, the value is the selected <option>'s.
    if (auto* sel = el->selectControl()) {
        auto opts = sel->getOptions();
        int idx = sel->selectedIndex();
        if (idx >= 0 && idx < static_cast<int>(opts.size())) return opts[idx].value;
        return std::string();
    }
    if (tagIs(el, "select", "SELECT")) {
        // No ElSelect yet (no layout pass has touched this element) — read
        // straight off the DOM instead of falling through to the generic
        // getAttribute("value") below, which is always empty for a <select>
        // (only its <option> children carry a value). Mirrors setFormValue's
        // DOM-attribute path so get/set stay consistent before first layout —
        // e.g. a headless script that creates a <select>, sets .value, and
        // reads it back without an intervening flush()/layout.
        dom::Element* first = nullptr;
        for (auto* child : el->children()) {
            if (!tagIs(child, "option", "OPTION")) continue;
            if (!first) first = child;
            if (child->hasAttribute("selected")) return optionValue(child);
        }
        return first ? optionValue(first) : std::string();
    }

    // <textarea>: the live value lives in the "value" attribute once any edit
    // has happened; before that, fall back to textContent (initial content from
    // HTML, e.g. `<textarea>foo</textarea>`).
    if (tagIs(el, "textarea", "TEXTAREA")) {
        if (el->hasAttribute("value")) return el->getAttribute("value");
        return el->textContent();
    }
    if (tagIs(el, "option", "OPTION")) {
        return optionValue(el);
    }
    return el->getAttribute("value");
}

void setFormValue(dom::Element* el, std::string_view value) {
    if (!el || !reflectsValue(el)) return;
    std::string s(value);

    // For <select>, sync the selection with the new value. We do BOTH:
    //   1. Stamp the `selected` attribute on the matching <option> (and clear
    //      it from siblings). This is what initSelectedIndex reads when the
    //      SelectControl is lazily created during the first layout pass —
    //      required because callers commonly run `select.value = "..."` at
    //      script-load time, before any render has triggered ElSelect
    //      construction.
    //   2. If the SelectControl already exists, also update its selectedIndex
    //      directly so the displayed selection moves immediately without
    //      waiting for a re-layout.
    if (tagIs(el, "select", "SELECT")) {
        int matchIdx = -1, idx = 0;
        for (auto* child : el->children()) {
            if (!tagIs(child, "option", "OPTION")) continue;
            std::string ov = optionValue(child);
            if (matchIdx < 0 && ov == s) {
                matchIdx = idx;
                child->setAttribute("selected", "");
            } else if (child->hasAttribute("selected")) {
                child->removeAttribute("selected");
            }
            ++idx;
        }
        if (auto* sel = el->selectControl(); sel && matchIdx >= 0)
            sel->setSelectedIndex(matchIdx);
        return;
    }

    // <textarea>: write to the "value" attribute, which is the storage shared
    // with the typing pipeline (handleKeyDown / handleTextInput). textContent
    // stays as the initial HTML content (defaultValue).
    if (tagIs(el, "textarea", "TEXTAREA")) {
        el->setAttribute("value", s);
        // A programmatic value write invalidates the control's undo history
        // (browser behavior — Ctrl+Z can't cross a script's rewrite). It also
        // re-arms `change`: the value moved, but nobody *changed* it in the
        // sense that event is about, so the next click away must be silent.
        if (auto* ta = el->textareaControl()) { ta->clearHistory(); ta->armChange(el); }
        return;
    }
    el->setAttribute("value", s);
    if (auto* inp = el->inputControl()) { inp->clearHistory(); inp->armChange(el); }
}

std::vector<dom::Element*> selectOptions(dom::Element* el) {
    std::vector<dom::Element*> out;
    if (el && tagIs(el, "select", "SELECT")) collectOptions(el, out);
    return out;
}

int selectedIndex(dom::Element* el) {
    if (!el) return -1;
    if (auto* sel = el->selectControl()) return sel->selectedIndex();
    // No layout control yet (the <select> hasn't been laid out): answer from
    // DOM state so a value set before the first frame round-trips, and so the
    // default matches what the control will adopt when it is created.
    if (!tagIs(el, "select", "SELECT")) return -1;
    if (el->hasPendingSelectedIndex()) return el->pendingSelectedIndex();
    int idx = -1, i = 0;
    for (auto* child : el->children()) {
        if (!tagIs(child, "option", "OPTION")) continue;
        if (idx < 0) idx = 0;                      // first option is the default
        if (child->hasAttribute("selected")) { idx = i; break; }
        ++i;
    }
    return idx;
}

void setSelectedIndex(dom::Element* el, int index) {
    if (!el) return;
    if (auto* sel = el->selectControl()) {
        sel->setSelectedIndex(index);
    } else if (tagIs(el, "select", "SELECT")) {
        // No layout control yet; remember the assignment so the control adopts
        // it on creation and the getter reflects it immediately.
        el->setPendingSelectedIndex(index);
        // ... and move the `selected` attribute with it, which is what
        // formValue() reads before the control exists. Without this,
        // `sel.selectedIndex = 1` followed by `sel.value` answers the index
        // that was set and the value of the option that wasn't — the two
        // properties disagreeing about one selection, and only until the first
        // layout pass, which is the worst kind of bug to reproduce.
        int i = 0;
        for (auto* child : el->children()) {
            if (!tagIs(child, "option", "OPTION")) continue;
            if (i == index) child->setAttribute("selected", "");
            else if (child->hasAttribute("selected")) child->removeAttribute("selected");
            ++i;
        }
    }
}

int tabIndex(const dom::Element* el) {
    if (!el) return -1;
    if (el->hasAttribute("tabindex")) {
        const std::string& v = el->getAttribute("tabindex");
        // An unparsable tabindex is as good as none — HTML says invalid values
        // fall back to the default, not to 0.
        char* end = nullptr;
        long n = std::strtol(v.c_str(), &end, 10);
        if (end != v.c_str()) return static_cast<int>(n);
    }
    static const char* const kFocusable[] = {
        "a", "area", "button", "input", "select", "textarea", "iframe",
        "summary", "audio", "video",
    };
    std::string t = el->tagName();
    for (char& c : t)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    for (const char* tag : kFocusable) {
        if (t != tag) continue;
        // <a> and <area> are focusable only WITH an href; a bare <a> used as a
        // styling hook is not in the tab order, and treating it as one is how a
        // page ends up with a hundred invisible tab stops.
        if ((t == "a" || t == "area") && !el->hasAttribute("href")) return -1;
        return 0;
    }
    return -1;
}

void setTabIndex(dom::Element* el, int value) {
    if (el) el->setAttribute("tabindex", std::to_string(value));
}

bool isRadioInput(dom::Element* el) {
    if (!el) return false;
    const std::string& tag = el->tagName();
    if (tag != "INPUT" && tag != "input") return false;
    return el->getAttribute("type") == "radio";
}

dom::Element* formOwnerOf(dom::Element* el) {
    for (auto* p = el ? el->parentElement() : nullptr; p; p = p->parentElement()) {
        const std::string& tag = p->tagName();
        if (tag == "FORM" || tag == "form") return p;
    }
    return nullptr;
}

void clearRadioGroup(dom::Element* el) {
    if (!isRadioInput(el)) return;
    const std::string name = el->getAttribute("name");
    if (name.empty()) return;
    auto* doc = el->document();
    auto* root = doc ? doc->documentElement() : nullptr;
    if (!root) return;
    auto* owner = formOwnerOf(el);
    for (auto* other : root->querySelectorAll("input[type=\"radio\"]")) {
        if (other == el) continue;
        if (other->getAttribute("name") != name) continue;
        if (formOwnerOf(other) != owner) continue;
        other->removeAttribute("checked");
    }
}

dom::Element* checkedRadioInGroup(dom::Element* el) {
    if (!isRadioInput(el)) return nullptr;
    if (el->hasAttribute("checked")) return el;
    const std::string name = el->getAttribute("name");
    if (name.empty()) return nullptr;
    auto* doc = el->document();
    auto* root = doc ? doc->documentElement() : nullptr;
    if (!root) return nullptr;
    auto* owner = formOwnerOf(el);
    for (auto* other : root->querySelectorAll("input[type=\"radio\"]")) {
        if (other == el) continue;
        if (other->getAttribute("name") != name) continue;
        if (formOwnerOf(other) != owner) continue;
        if (other->hasAttribute("checked")) return other;
    }
    return nullptr;
}

bool isLabelable(const dom::Element* el) {
    if (!el) return false;
    if (tagIs(el, "button", "BUTTON") ||
        tagIs(el, "select", "SELECT") ||
        tagIs(el, "textarea", "TEXTAREA") ||
        tagIs(el, "progress", "PROGRESS") ||
        tagIs(el, "meter", "METER") ||
        tagIs(el, "output", "OUTPUT")) {
        return true;
    }
    if (tagIs(el, "input", "INPUT")) {
        std::string t = el->getAttribute("type");
        for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return t != "hidden";
    }
    return false;
}

bool isInteractiveContent(const dom::Element* el) {
    if (!el) return false;
    if (tagIs(el, "button", "BUTTON") ||
        tagIs(el, "select", "SELECT") ||
        tagIs(el, "textarea", "TEXTAREA") ||
        tagIs(el, "details", "DETAILS") ||
        tagIs(el, "embed", "EMBED") ||
        tagIs(el, "iframe", "IFRAME")) {
        return true;
    }
    if (tagIs(el, "a", "A")) {
        return el->hasAttribute("href");
    }
    if (tagIs(el, "input", "INPUT")) {
        std::string t = el->getAttribute("type");
        for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return t != "hidden";
    }
    return false;
}

dom::Element* findLabeledControl(const dom::Element* label) {
    if (!label || !tagIs(label, "label", "LABEL")) return nullptr;

    if (label->hasAttribute("for")) {
        const std::string& forId = label->getAttribute("for");
        if (forId.empty()) return nullptr;
        if (auto* doc = label->document()) {
            dom::Element* target = doc->getElementById(forId);
            if (target && isLabelable(target)) return target;
        }
        return nullptr;
    }

    auto findFirstLabelable = [](auto& self, const dom::Element* el) -> dom::Element* {
        for (auto* child : el->children()) {
            if (isLabelable(child)) return child;
            if (auto* found = self(self, child)) return found;
        }
        return nullptr;
    };
    return findFirstLabelable(findFirstLabelable, label);
}

}  // namespace bro::layout
