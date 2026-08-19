// dataset proxy and classList / DOMTokenList handling.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "dom/element.h"
#include "dom/style_proxy.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

// ---------------------------------------------------------------------------
// classList
// ---------------------------------------------------------------------------
std::vector<std::string> classesOf(dom::Element* el) {
    std::vector<std::string> out;
    if (!el) return out;
    const std::string& s = el->className();
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

void writeClasses(dom::Element* el, const std::vector<std::string>& list) {
    std::string s;
    for (const std::string& c : list) {
        if (!s.empty()) s += ' ';
        s += c;
    }
    el->setClassName(s);
}

bool hasClass(dom::Element* el, const std::string& name) {
    for (const std::string& c : classesOf(el))
        if (c == name) return true;
    return false;
}

// A view over the element's `data-*` attributes:
// The name mapping is the web's: `data-user-id` <-> `dataset.userId`.
std::string datasetKeyToAttr(const std::string& key) {
    return "data-" + dom::StyleProxy::camelToKebab(key);
}

bool datasetAttrToKey(const std::string& attr, std::string* out) {
    if (attr.size() <= 5 || attr.compare(0, 5, "data-") != 0) return false;
    *out = dom::StyleProxy::kebabToCamel(attr.substr(5));
    return true;
}

}  // namespace

Value makeDatasetObject(HostNodeState* st) {
    HostProxyTraps t;
    t.get = [st](const std::string& key, Value& out) {
        if (!st->el) return false;
        const std::string attr = datasetKeyToAttr(key);
        // Absent answers undefined rather than "", which is what the web does
        // and what `if (el.dataset.role)` depends on.
        if (!st->el->hasAttribute(attr)) return false;
        out = ev::fromUtf8(st->el->getAttribute(attr));
        return true;
    };
    t.set = [st](const std::string& key, Value v) {
        if (!st->el || ev::isObject(v)) return;
        st->el->setAttribute(datasetKeyToAttr(key),
                             ev::isUndefined(v) ? "undefined" : ev::toUtf8(v));
    };
    t.has = [st](const std::string& key) {
        return st->el && st->el->hasAttribute(datasetKeyToAttr(key));
    };
    t.ownKeys = [st]() {
        std::vector<std::string> keys;
        if (!st->el) return keys;
        for (const auto& [name, value] : st->el->attributes()) {
            (void)value;
            std::string key;
            if (datasetAttrToKey(name, &key)) keys.push_back(key);
        }
        return keys;
    };
    t.remove = [st](const std::string& key) {
        if (st->el) st->el->removeAttribute(datasetKeyToAttr(key));
    };
    return makeHostProxy(std::move(t));
}

// Every mutator takes a variadic list, as DOMTokenList does: `classList.add(a, b)`
// is one call on the web and widget libraries write it that way.
Value makeClassListObject(HostNodeState* st) {
    ObjectBuilder b;
    b.def("add", 1, [st](Value, std::span<const Value> a) {
        if (!st->el) return ev::undefined();
        std::vector<std::string> list = classesOf(st->el);
        bool changed = false;
        for (const Value& v : a) {
            if (ev::isObject(v) || ev::isUndefined(v)) continue;
            std::string name = ev::toUtf8(v);
            if (name.empty()) continue;
            if (std::find(list.begin(), list.end(), name) == list.end()) {
                list.push_back(name);
                changed = true;
            }
        }
        if (changed) writeClasses(st->el, list);
        return ev::undefined();
    });
    b.def("remove", 1, [st](Value, std::span<const Value> a) {
        if (!st->el) return ev::undefined();
        std::vector<std::string> list = classesOf(st->el);
        size_t before = list.size();
        for (const Value& v : a) {
            if (ev::isObject(v) || ev::isUndefined(v)) continue;
            std::string name = ev::toUtf8(v);
            list.erase(std::remove(list.begin(), list.end(), name), list.end());
        }
        if (list.size() != before) writeClasses(st->el, list);
        return ev::undefined();
    });
    b.def("contains", 1, [st](Value, std::span<const Value> a) {
        Value v = argAt(a, 0);
        if (!st->el || ev::isObject(v) || ev::isUndefined(v))
            return ev::fromBool(false);
        return ev::fromBool(hasClass(st->el, ev::toUtf8(v)));
    });
    // The two-argument form is the one a UI actually calls —
    // `dom.classList.toggle('active', isActive)` — where the force argument
    // decides and the current state does not.
    b.def("toggle", 2, [st](Value, std::span<const Value> a) {
        Value v = argAt(a, 0);
        if (!st->el || ev::isObject(v) || ev::isUndefined(v))
            return ev::fromBool(false);
        std::string name = ev::toUtf8(v);
        bool present = hasClass(st->el, name);
        bool want = (a.size() > 1 && !ev::isUndefined(a[1])) ? ev::toBool(a[1])
                                                            : !present;
        if (want == present) return ev::fromBool(want);
        std::vector<std::string> list = classesOf(st->el);
        if (want) list.push_back(name);
        else list.erase(std::remove(list.begin(), list.end(), name), list.end());
        writeClasses(st->el, list);
        return ev::fromBool(want);
    });
    b.def("replace", 2, [st](Value, std::span<const Value> a) {
        Value oldV = argAt(a, 0), newV = argAt(a, 1);
        if (!st->el || ev::isObject(oldV) || ev::isObject(newV))
            return ev::fromBool(false);
        std::string oldName = ev::toUtf8(oldV), newName = ev::toUtf8(newV);
        std::vector<std::string> list = classesOf(st->el);
        auto it = std::find(list.begin(), list.end(), oldName);
        if (it == list.end()) return ev::fromBool(false);
        *it = newName;
        writeClasses(st->el, list);
        return ev::fromBool(true);
    });
    b.def("item", 1, [st](Value, std::span<const Value> a) {
        int idx = i32At(a, 0);
        std::vector<std::string> list = classesOf(st->el);
        if (idx < 0 || static_cast<size_t>(idx) >= list.size()) return ev::null();
        return ev::fromUtf8(list[static_cast<size_t>(idx)]);
    });
    b.accessor("length",
               [st](Value, std::span<const Value>) {
                   return ev::fromDouble(
                       static_cast<double>(classesOf(st->el).size()));
               },
               nullptr);
    b.accessor("value",
               [st](Value, std::span<const Value>) {
                   return ev::fromUtf8(st->el ? st->el->className() : std::string());
               },
               [st](Value, std::span<const Value> a) {
                   Value v = argAt(a, 0);
                   if (st->el && !ev::isObject(v) && !ev::isUndefined(v))
                       st->el->setClassName(ev::toUtf8(v));
                   return ev::undefined();
               });
    return b.get();
}

void decorateElementDataset(ObjectBuilder& b) {
    b.accessor("classList",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   if (!st->hasClassList && st->el) {
                       Value c = makeClassListObject(st);
                       st->classListObj.set(c);
                       st->hasClassList = true;
                   }
                   return st->classListObj.get();
               },
               nullptr);
    b.accessor("dataset",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   if (!st->hasDataset && st->el) {
                       Value d = makeDatasetObject(st);
                       st->datasetObj.set(d);
                       st->hasDataset = true;
                   }
                   return st->datasetObj.get();
               },
               nullptr);
}

}  // namespace bro::bronze_host
