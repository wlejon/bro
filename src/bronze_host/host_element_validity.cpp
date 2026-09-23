// Constraint validation and the HTMLFormElement methods.
//
// The computation is layout::form_validation — engine work, no realm needed.
// What is here is the two things that genuinely need one: turning HTML's
// `pattern` attribute into an ECMAScript RegExp (its spec'd dialect, not
// std::regex's), and shaping the report as the `validity` object a page reads.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"
#include "engine/element_activation.h"
#include "layout/form_validation.h"

#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

bool isFormTag(const dom::Element* el) {
    if (!el) return false;
    const std::string& t = el->tagName();
    return t == "FORM" || t == "form";
}

// HTML: the pattern is compiled as `^(?:<pattern>)$` with the `u` flag (the
// spec says `v`; `u` is the sanctioned approximation and what this engine's
// RegExp offers). An invalid pattern is IGNORED — the constraint matches
// everything — rather than failing the field, which would make a typo in the
// markup reject every value a user could type.
bool testPatternWithRegExp(const std::string& pattern, const std::string& value) {
    ev::GlobalValue reCtor = ev::globalValue("RegExp");
    if (!reCtor.found || !ev::isFunction(reCtor.value)) return true;
    ev::Persistent ctor(reCtor.value);
    // Every Value that has to outlive the next allocation is rooted as it is
    // made: `fromUtf8` collects, and a raw Value made before it is stale after.
    ev::Persistent source(ev::fromUtf8("^(?:" + pattern + ")$"));
    ev::Persistent flags(ev::fromUtf8("u"));

    // `u` first, then no flags: the spec asks for a Unicode-mode pattern, but
    // a Unicode-mode RegExp rejects escapes a non-Unicode one accepts, and a
    // pattern this engine cannot compile in that mode must fall back rather
    // than silently match everything.
    Value args[2] = { source.get(), flags.get() };
    ev::CallResult cr = ev::construct(ctor.get(), std::span<const Value>(args, 2));
    if (cr.thrown || !ev::isObject(cr.value)) {
        Value one[1] = { source.get() };
        cr = ev::construct(ctor.get(), std::span<const Value>(one, 1));
    }
    if (cr.thrown || !ev::isObject(cr.value)) return true;

    ev::Persistent re(cr.value);
    ev::Persistent test(ev::getProperty(re.get(), "test"));
    if (!ev::isFunction(test.get())) return true;
    ev::Persistent valArg(ev::fromUtf8(value));
    Value one[1] = { valArg.get() };
    ev::CallResult tr = ev::call(test.get(), re.get(), std::span<const Value>(one, 1));
    if (tr.thrown) return true;
    return ev::toBool(tr.value);
}

void fireInvalid(dom::Element* el) {
    dom::Event evt("invalid", false, true);
    evt.setIsTrusted(true);
    dom::dispatchDomEvent(el, evt);
}

// checkValidity() on a control: the report, plus an `invalid` event when it
// fails. On a <form> it means the form-wide walk instead — the element check
// run against the <form> itself is what always answered true, because a
// <form> has no value and no constraints of its own.
bool checkValidityOf(dom::Element* el) {
    if (!el) return true;
    if (isFormTag(el)) return engine::checkFormValidity(el);
    layout::ValidityReport r = layout::computeValidity(el);
    if (r.valid()) return true;
    fireInvalid(el);
    return false;
}

}  // namespace

void installHostPatternTester() {
    layout::setDefaultPatternTester(&testPatternWithRegExp);
}

void decorateElementValidity(ObjectBuilder& b) {
    b.def("setCustomValidity", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el) return ev::undefined();
        Value v = argAt(a, 0);
        if (ev::isObject(v)) return ev::undefined();
        st->el->setCustomValidity(hostNullableString(v));
        return ev::undefined();
    });

    b.def("checkValidity", 0, [](Value self_, std::span<const Value>) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        return ev::fromBool(checkValidityOf(st ? st->el : nullptr));
    });
    // reportValidity differs from checkValidity only by showing the message to
    // the user, and this engine has no native validation bubble to show.
    b.def("reportValidity", 0, [](Value self_, std::span<const Value>) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        return ev::fromBool(checkValidityOf(st ? st->el : nullptr));
    });

    b.accessor("validity",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   layout::ValidityReport r =
                       layout::computeValidity(st ? st->el : nullptr);
                   ObjectBuilder v;
                   v.set("valueMissing", ev::fromBool(r.valueMissing));
                   v.set("typeMismatch", ev::fromBool(r.typeMismatch));
                   v.set("patternMismatch", ev::fromBool(r.patternMismatch));
                   v.set("tooShort", ev::fromBool(r.tooShort));
                   v.set("tooLong", ev::fromBool(r.tooLong));
                   v.set("rangeUnderflow", ev::fromBool(r.rangeUnderflow));
                   v.set("rangeOverflow", ev::fromBool(r.rangeOverflow));
                   v.set("stepMismatch", ev::fromBool(r.stepMismatch));
                   v.set("badInput", ev::fromBool(r.badInput));
                   v.set("customError", ev::fromBool(r.customError));
                   v.set("valid", ev::fromBool(r.valid()));
                   return v.get();
               },
               nullptr);

    b.accessor("validationMessage",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st || !st->el) return ev::fromUtf8("");
                   layout::ValidityReport r = layout::computeValidity(st->el);
                   if (r.valid()) return ev::fromUtf8("");
                   return ev::fromUtf8(layout::defaultValidationMessage(r, st->el));
               },
               nullptr);

    b.accessor("willValidate",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   return ev::fromBool(st && layout::willValidate(st->el));
               },
               nullptr);

    // ---- HTMLFormElement ---------------------------------------------------
    // The controls a form owns, as an array that also answers to each
    // control's NAME — `form.elements.email` is how half the form code on the
    // web reaches a field. A real HTMLFormControlsCollection is a Proxy; a
    // snapshot with the named properties set on it covers what pages do.
    b.accessor("elements",
               [](Value self_, std::span<const Value>) -> Value {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st || !isFormTag(st->el)) return ev::undefined();
                   std::vector<dom::Element*> items;
                   layout::collectFormElements(st->el, items);
                   ev::Persistent arr(hostArrayOf(items.size(), [&items](size_t i) {
                       return hostElementValue(items[i]);
                   }));
                   for (dom::Element* c : items) {
                       const std::string& n = c->getAttribute("name");
                       if (n.empty()) continue;
                       // First control of a given name wins, as it does on the
                       // web; a later one must not shadow it.
                       if (!ev::isUndefined(ev::getProperty(arr.get(), n.c_str()))) continue;
                       ev::Persistent cv(hostElementValue(c));
                       arr.set(ev::setProperty(arr.get(), n.c_str(), cv.get()));
                   }
                   return arr.get();
               },
               nullptr);

    b.def("requestSubmit", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !isFormTag(st->el)) return ev::undefined();
        dom::Element* submitter = nullptr;
        if (!a.empty() && !ev::isUndefined(a[0]) && !ev::isNull(a[0])) {
            submitter = hostElementOf(a[0]);
        }
        engine::requestFormSubmit(st->el, submitter);
        return ev::undefined();
    });

    // submit() skips both constraint validation and the submit event, per
    // spec. bro has no native navigation, so there is nothing else it can do —
    // a page that wants a notification uses requestSubmit().
    b.def("submit", 0, [](Value, std::span<const Value>) { return ev::undefined(); });

    b.def("reset", 0, [](Value self_, std::span<const Value>) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !isFormTag(st->el)) return ev::undefined();
        engine::resetForm(st->el);
        return ev::undefined();
    });
}

}  // namespace bro::bronze_host
