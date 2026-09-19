// Form controls and reflection properties for DOM elements.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_anchor_download.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"
#include "engine/element_activation.h"
#include "engine/engine.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/form_control.h"
#include "dom/text_offsets.h"
#include "platform/dialogs.h"

#include <filesystem>
#include <initializer_list>
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

// Which tags actually REFLECT each link attribute. On every other tag the
// property is an ordinary expando — `div.href = x` must read back as x and
// must not put an href attribute on a <div>, which would make it match
// `[href]` selectors and, worse, would be lost entirely when the value is not
// a string (a page storing an object on `row.target` got "[object Object]").
bool tagIn(const std::string& tag, std::initializer_list<const char*> names) {
    std::string lower = tag;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* n : names) {
        if (lower == n) return true;
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
                   std::string v = st->el->getAttribute("draggable");
                   for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                   if (v == "true") return ev::fromBool(true);
                   if (v == "false") return ev::fromBool(false);
                   // HTML's two elements that are draggable with no attribute
                   // at all: an image, and a LINK (an <a> that has an href).
                   // A bare <a> is not one, which is why the check is on the
                   // attribute and not on the tag alone.
                   if (tagIn(st->el->tagName(), {"img"})) return ev::fromBool(true);
                   if (tagIn(st->el->tagName(), {"a"}) && st->el->hasAttribute("href"))
                       return ev::fromBool(true);
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
    defStrAttr("pattern", "pattern", "");
    defStrAttr("src", "src", "");
    defStrAttr("alt", "alt", "");
    defStrAttr("accept", "accept", "");
    defStrAttr("autocomplete", "autocomplete", "");

    // The link attributes reflect only on the tags HTML gives them to. On
    // anything else they are plain expandos — see tagIn() above for why that
    // matters, and note that this is also what keeps `div.download = fn` from
    // silently becoming the string "function () {...}".
    auto defLinkAttr = [&b](const char* name, const char* expando,
                            std::initializer_list<const char*> tags) {
        std::string a(name), x(expando);
        std::vector<std::string> owned(tags.begin(), tags.end());
        auto reflects = [owned](dom::Element* el) {
            std::string lower = el->tagName();
            for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (const std::string& t : owned) {
                if (lower == t) return true;
            }
            return false;
        };
        b.accessor(name,
                   [a, x, reflects](Value self_, std::span<const Value>) {
                       HostNodeState* st = hostNodeStateOfValue(self_);
                       if (!st || !st->el) return ev::undefined();
                       if (!reflects(st->el)) return ev::getProperty(self_, x.c_str());
                       return ev::fromUtf8(st->el->getAttribute(a));
                   },
                   [a, x, reflects](Value self_, std::span<const Value> args) {
                       HostNodeState* st = hostNodeStateOfValue(self_);
                       if (!st || !st->el) return ev::undefined();
                       Value v = argAt(args, 0);
                       if (!reflects(st->el)) {
                           ev::setProperty(self_, x.c_str(), v);
                           return ev::undefined();
                       }
                       if (!ev::isObject(v))
                           st->el->setAttribute(a, ev::isUndefined(v) ? "" : ev::toUtf8(v));
                       return ev::undefined();
                   });
    };
    defLinkAttr("href", "__broHref", {"a", "area", "link", "base"});
    defLinkAttr("download", "__broDownload", {"a", "area"});
    defLinkAttr("target", "__broTarget", {"a", "area", "form", "base"});
    defLinkAttr("rel", "__broRel", {"a", "area", "link"});

    // Numeric reflections. minLength/maxLength default to -1 ("no limit") and
    // size to 0, which is what a control-sizing helper branches on — not to
    // `undefined`, which made every such branch take the "no limit" path only
    // by accident of NaN comparisons.
    auto defNumAttr = [&b](const char* name, const char* attr, int fallback) {
        std::string a(attr);
        b.accessor(name,
                   [a, fallback](Value self_, std::span<const Value>) {
                       HostNodeState* st = hostNodeStateOfValue(self_);
                       if (!st || !st->el || !st->el->hasAttribute(a))
                           return ev::fromDouble(fallback);
                       try {
                           return ev::fromDouble(std::stoi(st->el->getAttribute(a)));
                       } catch (...) {
                           return ev::fromDouble(fallback);
                       }
                   },
                   [a](Value self_, std::span<const Value> args) {
                       HostNodeState* st = hostNodeStateOfValue(self_);
                       if (st && st->el)
                           st->el->setAttribute(a, std::to_string(i32At(args, 0)));
                       return ev::undefined();
                   });
    };
    defNumAttr("minLength", "minlength", -1);
    defNumAttr("maxLength", "maxlength", -1);
    defNumAttr("size", "size", 0);

    // The control's owning <form>: the one its `form` attribute names, else the
    // nearest <form> ancestor. Null when it has neither — which a validation
    // helper checks before it walks anything.
    b.accessor("form",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st || !st->el) return ev::null();
                   dom::Element* el = st->el;
                   const std::string& formId = el->getAttribute("form");
                   if (!formId.empty()) {
                       if (!el->document()) return ev::null();
                       dom::Element* owner = el->document()->getElementById(formId);
                       if (owner && tagIn(owner->tagName(), {"form"}))
                           return hostElementValue(owner);
                       return ev::null();
                   }
                   for (auto* p = el->parentElement(); p; p = p->parentElement()) {
                       if (tagIn(p->tagName(), {"form"})) return hostElementValue(p);
                   }
                   return ev::null();
               },
               nullptr);

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

    // click() is the whole activation behaviour, not a click event: focus
    // moves, a submit button submits its form, a summary opens its details, a
    // label forwards to its control. engine::clickElement is the one copy of
    // that, shared with the hit-tested path.
    b.def("click", 0, [](Value self_, std::span<const Value>) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el) return ev::undefined();
        engine::clickElement(st->el);
        return ev::undefined();
    });
}

}  // namespace bro::bronze_host
