// dataset proxy and classList / DOMTokenList handling.

#include "bronze_host/host_list_iteration.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_node.h"
#include "bronze_host/host_element.h"
#include "bronze_host/host_proxy.h"
#include "bronze_host/host_node_sweep.h"
#include "bronze_host/host_dom_lists.h"
#include "bronze_host/host_values.h"

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
std::vector<std::string> tokensOf(dom::Element* el, const std::string& attr) {
    std::vector<std::string> out;
    if (!el) return out;
    const std::string s = el->getAttribute(attr);
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i > start) {
            std::string tok = s.substr(start, i - start);
            // The token set is ordered and duplicate-free (DOM §7.1).
            if (std::find(out.begin(), out.end(), tok) == out.end()) out.push_back(std::move(tok));
        }
    }
    return out;
}

void writeTokens(dom::Element* el, const std::string& attr, const std::vector<std::string>& list) {
    std::string s;
    for (const std::string& c : list) {
        if (!s.empty()) s += ' ';
        s += c;
    }
    el->setAttribute(attr, s);
}

bool hasToken(dom::Element* el, const std::string& attr, const std::string& name) {
    for (const std::string& c : tokensOf(el, attr))
        if (c == name) return true;
    return false;
}

// A token argument: "" is SyntaxError and whitespace InvalidCharacterError
// (DOM §7.1 "validate a token").
std::string validToken(Value v) {
    std::string name = ev::isUndefined(v) ? std::string("undefined") : ev::toUtf8(v);
    if (name.empty())
        ev::throwValue(hostMakeDomError("SyntaxError", "The token provided must not be empty."));
    for (char c : name) {
        if (std::isspace(static_cast<unsigned char>(c)))
            ev::throwValue(hostMakeDomError("InvalidCharacterError",
                                            "The token provided ('" + name +
                                                "') contains HTML space characters, which are not valid in tokens."));
    }
    return name;
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

Value makeDatasetObject(HostNodeState* raw) {
    StateRef st(raw);
    HostProxyTraps t;
    t.proto = domStringMapProto();
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

// A DOMTokenList over one attribute of the element (classList: class,
// relList: rel). Every mutator takes a variadic list, as DOMTokenList does:
// `classList.add(a, b)` is one call on the web and widget libraries write it
// that way. The tokens are indexed properties too (`classList[0]`).
Value makeTokenListObject(HostNodeState* raw, const std::string& attr) {
    StateRef st(raw);
    ObjectBuilder b;
    b.def("add", 1, [st, attr](Value, std::span<const Value> a) {
        if (!st->el) return ev::undefined();
        std::vector<std::string> names;
        for (const Value& v : a) names.push_back(validToken(v));
        std::vector<std::string> list = tokensOf(st->el, attr);
        for (const std::string& name : names)
            if (std::find(list.begin(), list.end(), name) == list.end()) list.push_back(name);
        if (st->el->hasAttribute(attr) || !list.empty()) writeTokens(st->el, attr, list);
        return ev::undefined();
    });
    b.def("remove", 1, [st, attr](Value, std::span<const Value> a) {
        if (!st->el) return ev::undefined();
        std::vector<std::string> names;
        for (const Value& v : a) names.push_back(validToken(v));
        std::vector<std::string> list = tokensOf(st->el, attr);
        for (const std::string& name : names) list.erase(std::remove(list.begin(), list.end(), name), list.end());
        if (st->el->hasAttribute(attr)) writeTokens(st->el, attr, list);
        return ev::undefined();
    });
    b.def("contains", 1, [st, attr](Value, std::span<const Value> a) {
        Value v = argAt(a, 0);
        if (!st->el) return ev::fromBool(false);
        return ev::fromBool(hasToken(st->el, attr, ev::isUndefined(v) ? "undefined" : ev::toUtf8(v)));
    });
    // The two-argument form is the one a UI actually calls —
    // `dom.classList.toggle('active', isActive)` — where the force argument
    // decides and the current state does not.
    b.def("toggle", 2, [st, attr](Value, std::span<const Value> a) {
        const std::string name = validToken(argAt(a, 0));
        if (!st->el) return ev::fromBool(false);
        const bool present = hasToken(st->el, attr, name);
        const bool want = (a.size() > 1 && !ev::isUndefined(a[1])) ? ev::toBool(a[1]) : !present;
        if (want == present) return ev::fromBool(want);
        std::vector<std::string> list = tokensOf(st->el, attr);
        if (want) list.push_back(name);
        else list.erase(std::remove(list.begin(), list.end(), name), list.end());
        writeTokens(st->el, attr, list);
        return ev::fromBool(want);
    });
    b.def("replace", 2, [st, attr](Value, std::span<const Value> a) {
        const std::string oldName = validToken(argAt(a, 0));
        const std::string newName = validToken(argAt(a, 1));
        if (!st->el) return ev::fromBool(false);
        std::vector<std::string> list = tokensOf(st->el, attr);
        auto it = std::find(list.begin(), list.end(), oldName);
        if (it == list.end()) return ev::fromBool(false);
        if (std::find(list.begin(), list.end(), newName) != list.end()) list.erase(it);
        else *it = newName;
        writeTokens(st->el, attr, list);
        return ev::fromBool(true);
    });
    b.def("supports", 1, [](Value, std::span<const Value>) -> Value {
        return ev::throwTypeError("DOMTokenList has no supported tokens.");
    });
    b.def("item", 1, [st, attr](Value, std::span<const Value> a) {
        int idx = i32At(a, 0);
        std::vector<std::string> list = tokensOf(st->el, attr);
        if (idx < 0 || static_cast<size_t>(idx) >= list.size()) return ev::null();
        return ev::fromUtf8(list[static_cast<size_t>(idx)]);
    });
    b.accessor("length",
               [st, attr](Value, std::span<const Value>) {
                   return ev::fromDouble(static_cast<double>(tokensOf(st->el, attr).size()));
               },
               nullptr);
    auto value = [st, attr](Value, std::span<const Value>) {
        return ev::fromUtf8(st->el ? st->el->getAttribute(attr) : std::string());
    };
    b.accessor("value", value, [st, attr](Value, std::span<const Value> a) {
        Value v = argAt(a, 0);
        if (st->el) st->el->setAttribute(attr, ev::isUndefined(v) ? "undefined" : ev::toUtf8(v));
        return ev::undefined();
    });
    b.def("toString", 0, value);
    // DOMTokenList is iterable<DOMString>: for…of, spread, forEach.
    installSnapshotIteration(b.get(), [st, attr]() {
        std::vector<std::string> list = tokensOf(st->el, attr);
        return hostArrayOf(list.size(), [&list](size_t i) { return ev::fromUtf8(list[i]); });
    });

    HostProxyTraps t;
    t.methods = b.get();
    t.proto = domTokenListProto();
    auto index = [](const std::string& key, size_t& out) {
        if (key.empty() || key.size() > 9) return false;
        for (char c : key)
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        if (key.size() > 1 && key[0] == '0') return false;
        out = static_cast<size_t>(std::stoul(key));
        return true;
    };
    t.get = [st, attr, index](const std::string& key, Value& out) {
        size_t i = 0;
        if (!index(key, i)) return false;
        std::vector<std::string> list = tokensOf(st->el, attr);
        if (i >= list.size()) return false;
        out = ev::fromUtf8(list[i]);
        return true;
    };
    t.has = [st, attr, index](const std::string& key) {
        size_t i = 0;
        return index(key, i) && i < tokensOf(st->el, attr).size();
    };
    t.ownKeys = [st, attr]() {
        std::vector<std::string> keys;
        const size_t n = tokensOf(st->el, attr).size();
        for (size_t i = 0; i < n; ++i) keys.push_back(std::to_string(i));
        return keys;
    };
    return makeHostProxy(std::move(t));
}

void decorateElementDataset(ObjectBuilder& b) {
    b.accessor("classList",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   if (!st->hasClassList && st->el) {
                       Value c = makeTokenListObject(st, "class");
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
