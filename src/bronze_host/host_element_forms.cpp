// Form controls and reflection properties for DOM elements.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "dom/element.h"
#include "js/dom_bindings.h"
#include "layout/form_control.h"

#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {
// Where a non-reflecting element's `value` actually lives. Same key as the
// QuickJS binding's, so one <div> has one `value` whichever realm wrote it.
constexpr const char* kValueExpando = "__broValue";
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
    // The fallback is src/js/element_bindings.cpp's, down to the key: both
    // realms bind the same elements, and a page that sets `div.value` in one
    // and reads it in the other must see one property and not two.
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
                   if (!ev::isObject(v))
                       layout::setFormValue(st->el,
                                            ev::isUndefined(v) ? "" : ev::toUtf8(v));
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

    // `checked` writes go through js::clearRadioGroup rather than straight to
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
                       js::clearRadioGroup(st->el);
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
    defBoolAttr("draggable", "draggable");

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
}

}  // namespace bro::bronze_host
