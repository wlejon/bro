// The event plumbing the host objects that fire events share (Image,
// XMLHttpRequest): the `on<type>` slot, the addEventListener list, and the
// dispatch that runs both.
//
// WHERE THE LISTENERS LIVE, and why it is not host memory: a host-side table
// would have to hold each callback in an ev::Persistent, and it would have to
// be freed when the object dies — which means the object's handle finalizer
// would own the Persistent. embed.h forbids exactly that: a finalizer runs
// mid-collection and may not call back into the embed API, and ~Persistent is
// the embed API. So the list is an ordinary property on the object itself. It
// dies with the object, the collector does the freeing, and no finalizer ever
// touches a root slot.
//
// The list is a plain object with numeric keys and a `length` rather than a JS
// Array, because the embed API builds plain objects and has no array
// constructor. Nothing JS-visible reads it, so the shape is private.

#include "bronze_host/host_internal.h"

#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

// Own and enumerable — embed::setProperty defines exactly one kind of property
// — so this name is visible to Object.keys on the object. Nothing in the
// three.js loader path enumerates an image or a request, and the prefix makes
// an accidental collision with app data a deliberate act.
std::string listenerKey(const std::string& type) {
    return "__bronzeHostListeners_" + type;
}

uint32_t listLength(ev::Persistent& list) {
    Value lenV = ev::getProperty(list.get(), "length");
    if (ev::isObject(lenV) || ev::isUndefined(lenV)) return 0;
    double d = ev::toDouble(lenV);
    if (!(d > 0.0)) return 0;  // NaN and negatives answer 0, not a huge cast
    return static_cast<uint32_t>(d);
}

void setListLength(ev::Persistent& list, uint32_t n) {
    Value len = ev::fromDouble(n);
    list.set(ev::setProperty(list.get(), "length", len));
}

}  // namespace

void addHostListener(ev::Persistent& obj, const std::string& type, Value fn) {
    if (!ev::isFunction(fn)) return;
    // fn must survive the getProperty below, which allocates the key string.
    ev::Persistent fnP(fn);

    const std::string key = listenerKey(type);
    ev::Persistent list(ev::getProperty(obj.get(), key));
    if (!ev::isObject(list.get())) {
        list.set(ev::createObject());
        setListLength(list, 0);
        obj.set(ev::setProperty(obj.get(), key, list.get()));
    }

    // A repeat registration of the same callback is a no-op on the web, and
    // three.js's ImageLoader can reach one when two loads share an image.
    const uint32_t n = listLength(list);
    for (uint32_t i = 0; i < n; ++i) {
        Value existing = ev::getElement(list.get(), i);
        if (ev::toBits(existing) == ev::toBits(fnP.get())) return;
    }

    list.set(ev::setElement(list.get(), n, fnP.get()));
    setListLength(list, n + 1);
}

void removeHostListener(ev::Persistent& obj, const std::string& type, Value fn) {
    if (!ev::isFunction(fn)) return;
    ev::Persistent fnP(fn);

    ev::Persistent list(ev::getProperty(obj.get(), listenerKey(type)));
    if (!ev::isObject(list.get())) return;

    const uint32_t n = listLength(list);
    uint32_t found = n;
    for (uint32_t i = 0; i < n; ++i) {
        Value existing = ev::getElement(list.get(), i);
        if (ev::toBits(existing) == ev::toBits(fnP.get())) {
            found = i;
            break;
        }
    }
    if (found == n) return;

    // Compact in place: the list is small (one or two entries in every path
    // three.js takes), so a shift beats a tombstone that dispatch would have to
    // skip.
    for (uint32_t i = found + 1; i < n; ++i) {
        Value moved = ev::getElement(list.get(), i);
        list.set(ev::setElement(list.get(), i - 1, moved));
    }
    Value gone = ev::undefined();
    list.set(ev::setElement(list.get(), n - 1, gone));
    setListLength(list, n - 1);
}

void dispatchHostEvent(ev::Persistent target, const std::string& type) {
    // Snapshot every handler BEFORE calling any of them. three.js's ImageLoader
    // removes its own load and error listeners from inside the handler it is
    // running, so a dispatch that re-read the list between calls would skip the
    // sibling that just shifted down.
    std::vector<ev::Persistent> handlers;
    {
        Value on = ev::getProperty(target.get(), "on" + type);
        if (ev::isFunction(on)) handlers.emplace_back(on);
    }
    {
        ev::Persistent list(ev::getProperty(target.get(), listenerKey(type)));
        if (ev::isObject(list.get())) {
            const uint32_t n = listLength(list);
            for (uint32_t i = 0; i < n; ++i) {
                Value entry = ev::getElement(list.get(), i);
                if (ev::isFunction(entry)) handlers.emplace_back(entry);
            }
        }
    }
    if (handlers.empty()) return;

    for (ev::Persistent& handler : handlers) {
        // One event object per handler: built AFTER the previous call returned,
        // so no Value read out of it is ever held across a call into compiled
        // code. Cheaper to allocate than to root across the loop.
        ev::Persistent evt(ev::createObject());
        Value typeV = ev::fromUtf8(type);
        evt.set(ev::setProperty(evt.get(), "type", typeV));
        evt.set(ev::setProperty(evt.get(), "target", target.get()));

        Value arg = evt.get();
        ev::CallResult r =
            ev::call(handler.get(), target.get(), std::span<const Value>(&arg, 1));
        // Report and keep going: one broken handler must not silence its
        // siblings — the same stance the rAF and window-listener paths take.
        if (r.thrown) reportBronzeError(("event " + type).c_str(), r.value);
    }
}

}  // namespace bro::bronze_host
