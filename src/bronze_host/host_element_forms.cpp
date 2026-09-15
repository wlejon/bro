// Form controls and reflection properties for DOM elements.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_anchor_download.h"

#include "dom/element.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"
#include "engine/engine.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/form_control.h"
#include "dom/text_offsets.h"
#include "platform/dialogs.h"

#include <filesystem>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {
// Where a non-reflecting element's `value` actually lives.
constexpr const char* kValueExpando = "__broValue";

static std::string selectionValueOf(dom::Element* el) {
    if (!el) return std::string();
    const std::string& tag = el->tagName();
    if (tag == "TEXTAREA" || tag == "textarea") {
        if (el->hasAttribute("value")) return el->getAttribute("value");
        return el->textContent();
    }
    return el->getAttribute("value");
}

static void performElementClick(dom::Element* el) {
    if (!el || el->hasAttribute("disabled")) return;
    const std::string& tag = el->tagName();
    if (tag == "LABEL" || tag == "label") {
        dom::MouseEvent ev("click");
        dom::dispatchDomEvent(el, ev);
        if (ev.defaultPrevented()) return;
        dom::Element* control = layout::findLabeledControl(el);
        if (control && !control->hasAttribute("disabled")) {
            performElementClick(control);
        }
        return;
    }

    bool isInput = (tag == "INPUT" || tag == "input");
    bool isAnchor = (tag == "A" || tag == "a");
    dom::MouseEvent ev("click");
    dom::dispatchDomEvent(el, ev);
    if (!el) return;
    if (!ev.defaultPrevented() && isAnchor) {
        runAnchorDownload(el);
    }
    if (!ev.defaultPrevented() && isInput) {
        std::string t = el->getAttribute("type");
        for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (t == "radio") {
            if (!el->hasAttribute("checked")) {
                layout::clearRadioGroup(el);
                if (!el) return;
                el->setAttribute("checked", "");
                dom::Event inputEvt("input");
                dom::dispatchDomEvent(el, inputEvt);
                if (!el) return;
                dom::Event changeEvt("change");
                dom::dispatchDomEvent(el, changeEvt);
            }
        } else if (t == "checkbox") {
            if (el->hasAttribute("checked")) el->removeAttribute("checked");
            else el->setAttribute("checked", "");
            dom::Event inputEvt("input");
            dom::dispatchDomEvent(el, inputEvt);
            if (!el) return;
            dom::Event changeEvt("change");
            dom::dispatchDomEvent(el, changeEvt);
        } else if (t == "file") {
            bool allowMultiple = el->hasAttribute("multiple");
            std::string accept = el->getAttribute("accept");
            std::vector<std::string> picked = platform::Dialogs::pickFiles(accept, allowMultiple);
            if (!picked.empty()) {
                el->setSelectedFiles(picked);
                std::string filename = std::filesystem::path(picked[0]).filename().string();
                el->setAttribute("value", "C:\\fakepath\\" + filename);
                dom::Event inputEvt("input");
                dom::dispatchDomEvent(el, inputEvt);
                if (!el) return;
                dom::Event changeEvt("change");
                dom::dispatchDomEvent(el, changeEvt);
            }
        }
    }
}

static bool checkPatternMismatch(dom::Element* el) {
    if (!el || !el->hasAttribute("pattern")) return false;
    std::string pattern = el->getAttribute("pattern");
    std::string val = layout::formValue(el);
    if (val.empty() || pattern.empty()) return false;
    ev::GlobalValue reCtor = ev::globalValue("RegExp");
    if (reCtor.found && ev::isFunction(reCtor.value)) {
        Value args[2] = { ev::fromUtf8("^(?:" + pattern + ")$"), ev::fromUtf8("u") };
        ev::CallResult cr = ev::call(reCtor.value, ev::undefined(), std::span<const Value>(args, 2));
        if (!cr.thrown && ev::isObject(cr.value)) {
            Value testFn = ev::getProperty(cr.value, "test");
            if (ev::isFunction(testFn)) {
                Value valArg[1] = { ev::fromUtf8(val) };
                ev::CallResult tr = ev::call(testFn, cr.value, std::span<const Value>(valArg, 1));
                if (!tr.thrown && !ev::toBool(tr.value)) {
                    return true;
                }
            }
        }
    }
    return false;
}
} // namespace

void decorateElementForms(ObjectBuilder& b) {
    // `value` is on the ONE element prototype, so it is reached by every
    // element and not only by the controls that reflect one. A <div>'s
    // `.value` is an ordinary expando (form_control.h says so at
    // reflectsValue), and an accessor that answers "" for it is not merely
    // imprecise: the setter swallows the write. That cost real time to find —
    // three.js's editor stores an object id on each outliner row as
    // `option.value = object.id`, reads it back in the row's click handler,
    // and got "" instead, so `parseInt` made a NaN, `getObjectById` answered
    // undefined, and selecting anything threw on `object.uuid`.
    //
    // The fallback is stored on an expando property so non-form elements
    // setting `div.value` can read it back cleanly.
    b.accessor("value",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   if (!st->el) return ev::fromUtf8(std::string());
                   if (!layout::reflectsValue(st->el))
                       return ev::getProperty(self_, kValueExpando);
                   return ev::fromUtf8(layout::formValue(st->el));
               },
               [](Value self_, std::span<const Value> a) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st || !st->el) return ev::undefined();
                   Value v = argAt(a, 0);
                   if (!layout::reflectsValue(st->el)) {
                       // Whatever it is — number, object, undefined — kept as
                       // it was given, because an expando is not coerced.
                       ev::setProperty(self_, kValueExpando, v);
                       return ev::undefined();
                   }
                    if (!ev::isObject(v)) {
                        std::string s = ev::isUndefined(v) ? "" : ev::toUtf8(v);
                        layout::setFormValue(st->el, s);
                        if (s.empty() && (st->el->tagName() == "INPUT" || st->el->tagName() == "input")) {
                            std::string t = st->el->getAttribute("type");
                            for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            if (t == "file") {
                                st->el->setSelectedFiles({});
                            }
                        }
                    }
                    return ev::undefined();
                });

    b.accessor("selectedIndex",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   return ev::fromDouble(st->el ? layout::selectedIndex(st->el) : -1);
               },
               [](Value self_, std::span<const Value> a) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   if (st->el) layout::setSelectedIndex(st->el, i32At(a, 0));
                   return ev::undefined();
               });

    b.accessor("options",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   if (!st->el) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
                   std::vector<dom::Element*> opts = layout::selectOptions(st->el);
                   return hostArrayOf(opts.size(), [&opts](size_t i) {
                       return hostElementValue(opts[i]);
                   });
               },
               nullptr);

    // `checked` writes go through layout::clearRadioGroup rather than straight to
    // the attribute: a radio's group is cleared however its checkedness became
    // true, not only by a click, and leaving the old member checked would show
    // two picked radios in a group that can only mean one.
    b.accessor("checked",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   return ev::fromBool(st->el && st->el->hasAttribute("checked"));
               },
               [](Value self_, std::span<const Value> a) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   if (!st->el) return ev::undefined();
                   if (ev::toBool(argAt(a, 0))) {
                       layout::clearRadioGroup(st->el);
                       st->el->setAttribute("checked", "");
                   } else {
                       st->el->removeAttribute("checked");
                   }
                   return ev::undefined();
               });

    // Boolean HTML attributes: PRESENCE means true, whatever the value, so
    // `<input disabled>` (which parses to disabled="") counts. Writing false
    // removes the attribute rather than setting it to "false", which would
    // read back as true.
    auto defBoolAttr = [&b](const char* name, const char* attr) {
        std::string a(attr);
        b.accessor(name,
                   [a](Value self_, std::span<const Value>) {
                       HostNodeState* st = hostNodeStateOfValue(self_);
                       return ev::fromBool(st && st->el && st->el->hasAttribute(a));
                   },
                   [a](Value self_, std::span<const Value> args) {
                       HostNodeState* st = hostNodeStateOfValue(self_);
                       if (!st || !st->el) return ev::undefined();
                       if (ev::toBool(argAt(args, 0))) st->el->setAttribute(a, "");
                       else st->el->removeAttribute(a);
                       return ev::undefined();
                   });
    };
    defBoolAttr("disabled", "disabled");
    defBoolAttr("selected", "selected");
    defBoolAttr("multiple", "multiple");
    defBoolAttr("required", "required");
    defBoolAttr("readOnly", "readonly");
    defBoolAttr("hidden", "hidden");
    defBoolAttr("autofocus", "autofocus");

    b.accessor("draggable",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st || !st->el) return ev::fromBool(false);
                   const std::string& v = st->el->getAttribute("draggable");
                   if (v == "true") return ev::fromBool(true);
                   if (v == "false") return ev::fromBool(false);
                   if (st->el->tagName() == "IMG" || st->el->tagName() == "img") return ev::fromBool(true);
                   return ev::fromBool(false);
               },
               [](Value self_, std::span<const Value> args) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st || !st->el) return ev::undefined();
                   bool bval = ev::toBool(argAt(args, 0));
                   st->el->setAttribute("draggable", bval ? "true" : "false");
                   return ev::undefined();
               });

    // Plain string reflections. `type` is the one with a default — an <input>
    // with no type attribute is a text input, and UI code branches on it.
    auto defStrAttr = [&b](const char* name, const char* attr,
                               const char* fallback) {
        std::string a(attr), f(fallback);
        b.accessor(name,
                   [a, f](Value self_, std::span<const Value>) {
                       HostNodeState* st = hostNodeStateOfValue(self_);
                       if (!st || !st->el) return ev::fromUtf8("");
                       const std::string& v = st->el->getAttribute(a);
                       return ev::fromUtf8(v.empty() ? f : v);
                   },
                   [a](Value self_, std::span<const Value> args) {
                       HostNodeState* st = hostNodeStateOfValue(self_);
                       Value v = argAt(args, 0);
                       if (st && st->el && !ev::isObject(v))
                           st->el->setAttribute(a, ev::isUndefined(v) ? ""
                                                                      : ev::toUtf8(v));
                       return ev::undefined();
                   });
    };
    defStrAttr("type", "type", "text");
    defStrAttr("name", "name", "");
    defStrAttr("placeholder", "placeholder", "");
    defStrAttr("title", "title", "");
    defStrAttr("min", "min", "");
    defStrAttr("max", "max", "");
    defStrAttr("step", "step", "");
    defStrAttr("href", "href", "");
    defStrAttr("download", "download", "");
    defStrAttr("target", "target", "");
    defStrAttr("rel", "rel", "");
    defStrAttr("src", "src", "");
    defStrAttr("alt", "alt", "");
    defStrAttr("accept", "accept", "");
    defStrAttr("autocomplete", "autocomplete", "");

    // tabIndex is a number whose default depends on the tag, so it goes through
    // layout::tabIndex rather than reading the attribute here — see
    // form_control.h for why a flat default is wrong in both directions.
    b.accessor("tabIndex",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   return ev::fromDouble(st->el ? layout::tabIndex(st->el) : -1);
               },
               [](Value self_, std::span<const Value> a) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   if (st->el) layout::setTabIndex(st->el, i32At(a, 0));
                   return ev::undefined();
               });

    // ---- Selection & Validity ---------------------------------------------
    b.def("select", 0, [](Value self_, std::span<const Value>) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el) return ev::undefined();
        if (auto* inp = st->el->inputControl()) inp->selectAll();
        else if (auto* ta = st->el->textareaControl()) ta->selectAll();
        return ev::undefined();
    });

    b.def("setSelectionRange", 2, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el || a.size() < 2) return ev::undefined();
        int start = static_cast<int>(ev::toDouble(a[0]));
        int end = static_cast<int>(ev::toDouble(a[1]));
        const std::string val = selectionValueOf(st->el);
        const int bs = dom::utf16ToUtf8Byte(val, start);
        const int be = dom::utf16ToUtf8Byte(val, end);
        if (auto* inp = st->el->inputControl()) inp->setSelectionRange(bs, be);
        else if (auto* ta = st->el->textareaControl()) ta->setSelectionRange(bs, be);
        return ev::undefined();
    });

    b.accessor("selectionStart",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st || !st->el) return ev::null();
                   const std::string val = selectionValueOf(st->el);
                   if (auto* inp = st->el->inputControl())
                       return ev::fromDouble(dom::utf8ByteToUtf16(val, inp->selectionStart()));
                   if (auto* ta = st->el->textareaControl())
                       return ev::fromDouble(dom::utf8ByteToUtf16(val, ta->selectionStart()));
                   return ev::null();
               },
               [](Value self_, std::span<const Value> a) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st || !st->el || a.empty()) return ev::undefined();
                   int start = static_cast<int>(ev::toDouble(a[0]));
                   const std::string val = selectionValueOf(st->el);
                   const int b = dom::utf16ToUtf8Byte(val, start);
                   if (auto* inp = st->el->inputControl())
                       inp->setSelectionRange(b, inp->selectionEnd() < b ? b : inp->selectionEnd());
                   else if (auto* ta = st->el->textareaControl())
                       ta->setSelectionRange(b, ta->selectionEnd() < b ? b : ta->selectionEnd());
                   return ev::undefined();
               });

    b.accessor("selectionEnd",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st || !st->el) return ev::null();
                   const std::string val = selectionValueOf(st->el);
                   if (auto* inp = st->el->inputControl())
                       return ev::fromDouble(dom::utf8ByteToUtf16(val, inp->selectionEnd()));
                   if (auto* ta = st->el->textareaControl())
                       return ev::fromDouble(dom::utf8ByteToUtf16(val, ta->selectionEnd()));
                   return ev::null();
               },
               [](Value self_, std::span<const Value> a) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st || !st->el || a.empty()) return ev::undefined();
                   int end = static_cast<int>(ev::toDouble(a[0]));
                   const std::string val = selectionValueOf(st->el);
                   const int b = dom::utf16ToUtf8Byte(val, end);
                   if (auto* inp = st->el->inputControl())
                       inp->setSelectionRange(inp->selectionStart(), b);
                   else if (auto* ta = st->el->textareaControl())
                       ta->setSelectionRange(ta->selectionStart(), b);
                   return ev::undefined();
               });

    b.def("setCustomValidity", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el || a.empty() || ev::isUndefined(a[0])) return ev::undefined();
        st->el->setCustomValidity(ev::toUtf8(a[0]));
        return ev::undefined();
    });

    auto checkVal = [](HostNodeState* st) -> bool {
        if (!st || !st->el) return true;
        bool valid = st->el->customValidity().empty();
        if (valid && st->el->hasAttribute("required")) {
            std::string val = layout::formValue(st->el);
            if (val.empty()) valid = false;
        }
        if (valid && checkPatternMismatch(st->el)) {
            valid = false;
        }
        if (!valid) {
            dom::Event evt("invalid", false, true);
            evt.setIsTrusted(true);
            if (auto* eng = hostEngine()) eng->dispatchElementEvent(st->el, evt);
        }
        return valid;
    };

    b.def("checkValidity", 0, [checkVal](Value self_, std::span<const Value>) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        return ev::fromBool(checkVal(st));
    });

    b.def("reportValidity", 0, [checkVal](Value self_, std::span<const Value>) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        return ev::fromBool(checkVal(st));
    });

    b.accessor("validity",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   ObjectBuilder v;
                   bool customError = st && st->el && !st->el->customValidity().empty();
                   bool valueMissing = false;
                   if (st && st->el && st->el->hasAttribute("required")) {
                       if (layout::formValue(st->el).empty()) valueMissing = true;
                   }
                   bool patternMismatch = st && st->el ? checkPatternMismatch(st->el) : false;
                   bool valid = !customError && !valueMissing && !patternMismatch;
                   v.set("valid", ev::fromBool(valid));
                   v.set("customError", ev::fromBool(customError));
                   v.set("valueMissing", ev::fromBool(valueMissing));
                   v.set("typeMismatch", ev::fromBool(false));
                   v.set("patternMismatch", ev::fromBool(patternMismatch));
                   v.set("tooLong", ev::fromBool(false));
                   v.set("tooShort", ev::fromBool(false));
                   v.set("rangeUnderflow", ev::fromBool(false));
                   v.set("rangeOverflow", ev::fromBool(false));
                   v.set("stepMismatch", ev::fromBool(false));
                   v.set("badInput", ev::fromBool(false));
                   return v.get();
               },
               nullptr);

    b.accessor("validationMessage",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st || !st->el) return ev::fromUtf8("");
                   if (!st->el->customValidity().empty())
                       return ev::fromUtf8(st->el->customValidity());
                   if (st->el->hasAttribute("required") && layout::formValue(st->el).empty())
                       return ev::fromUtf8("Please fill out this field.");
                   if (checkPatternMismatch(st->el))
                       return ev::fromUtf8("Please match the requested format.");
                   return ev::fromUtf8("");
               },
               nullptr);

    b.accessor("willValidate",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st || !st->el) return ev::fromBool(false);
                   return ev::fromBool(layout::reflectsValue(st->el) && !st->el->hasAttribute("disabled"));
               },
               nullptr);

    // files: the FileList of an <input type=file> — the paths the picker (or
    // headless's setPickedFiles) returned, as real File objects with the
    // non-standard `.path` (host_file_path.cpp; docs/file-api.js). Null for
    // any other element, per spec, and empty until the user picks something,
    // which is what a page checks first. The same helper the drop path uses,
    // so a picked file and a dropped one are the same kind of object.
    b.accessor("files",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st || !st->el) return ev::null();
                   const std::string& tag = st->el->tagName();
                   if (tag != "INPUT" && tag != "input") return ev::null();
                   std::string t = st->el->getAttribute("type");
                   for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                   if (t != "file") return ev::null();
                   const std::vector<std::string> paths = st->el->selectedFiles();
                   return hostArrayOf(paths.size(), [&paths](size_t i) {
                       return makeFileOrDescriptorFromPath(paths[i]);
                   });
               },
               nullptr);

    b.def("click", 0, [](Value self_, std::span<const Value>) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el) return ev::undefined();
        performElementClick(st->el);
        return ev::undefined();
    });
}

}  // namespace bro::bronze_host
