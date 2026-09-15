#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "engine/engine.h"
#include "engine/menu_bar.h"

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::bronze_host {

namespace {

static engine::MenuBar::Item parseMenuItem(Value v) {
    engine::MenuBar::Item item;
    if (!ev::isObject(v)) return item;

    Value idVal = ev::getProperty(v, "id");
    if (ev::isString(idVal)) item.id = ev::toUtf8(idVal);

    Value labelVal = ev::getProperty(v, "label");
    if (ev::isString(labelVal)) item.label = ev::toUtf8(labelVal);

    Value accelVal = ev::getProperty(v, "accel");
    if (ev::isString(accelVal)) item.accel = ev::toUtf8(accelVal);

    Value sepVal = ev::getProperty(v, "separator");
    if (!ev::isUndefined(sepVal)) item.separator = ev::toBool(sepVal);

    Value enVal = ev::getProperty(v, "enabled");
    if (!ev::isUndefined(enVal)) item.enabled = ev::toBool(enVal);

    Value hidVal = ev::getProperty(v, "hidden");
    if (!ev::isUndefined(hidVal)) item.hidden = ev::toBool(hidVal);

    Value chkVal = ev::getProperty(v, "checked");
    if (!ev::isUndefined(chkVal)) item.checked = ev::toBool(chkVal);

    Value itemsVal = ev::getProperty(v, "items");
    if (hostIsArray(itemsVal)) {
        uint32_t len = static_cast<uint32_t>(ev::toDouble(ev::getProperty(itemsVal, "length")));
        for (uint32_t i = 0; i < len; ++i) {
            item.children.push_back(parseMenuItem(ev::getElement(itemsVal, i)));
        }
    }
    return item;
}

static Value menuItemToJS(const engine::MenuBar::Item& item) {
    ObjectBuilder b;
    if (!item.id.empty()) b.set("id", ev::fromUtf8(item.id));
    if (!item.label.empty()) b.set("label", ev::fromUtf8(item.label));
    if (!item.accel.empty()) b.set("accel", ev::fromUtf8(item.accel));
    if (item.separator) b.set("separator", ev::fromBool(true));
    b.set("enabled", ev::fromBool(item.enabled));
    b.set("hidden", ev::fromBool(item.hidden));
    b.set("checked", ev::fromBool(item.checked));
    if (!item.children.empty()) {
        Value arr = hostArrayOf(item.children.size(), [&item](size_t i) -> Value {
            return menuItemToJS(item.children[i]);
        });
        b.set("items", arr);
    }
    return b.get();
}

static std::unordered_map<std::string, ev::Persistent> s_menuHandlers;

} // namespace

Value makeBroMenuValue() {
    ObjectBuilder menu;

    menu.def("show", 0, [](Value, std::span<const Value>) -> Value {
        if (auto* eng = hostEngine()) {
            eng->menuBar().visible = true;
            eng->menuBar().dirty = true;
            eng->onMenuChanged();
        }
        return ev::undefined();
    });

    menu.def("hide", 0, [](Value, std::span<const Value>) -> Value {
        if (auto* eng = hostEngine()) {
            eng->menuBar().visible = false;
            eng->menuBar().dirty = true;
            eng->onMenuChanged();
        }
        return ev::undefined();
    });

    menu.accessor("visible",
        [](Value, std::span<const Value>) -> Value {
            auto* eng = hostEngine();
            return ev::fromBool(eng ? eng->menuBar().visible : false);
        },
        [](Value, std::span<const Value> a) -> Value {
            if (auto* eng = hostEngine()) {
                if (!a.empty()) {
                    eng->menuBar().visible = ev::toBool(a[0]);
                    eng->menuBar().dirty = true;
                    eng->onMenuChanged();
                }
            }
            return ev::undefined();
        });

    menu.def("set", 1, [](Value, std::span<const Value> a) -> Value {
        auto* eng = hostEngine();
        if (!eng) return ev::undefined();
        std::vector<engine::MenuBar::Item> roots;
        if (!a.empty() && hostIsArray(a[0])) {
            Value arr = a[0];
            uint32_t len = static_cast<uint32_t>(ev::toDouble(ev::getProperty(arr, "length")));
            for (uint32_t i = 0; i < len; ++i) {
                roots.push_back(parseMenuItem(ev::getElement(arr, i)));
            }
        }
        eng->menuBar().setRoots(std::move(roots));
        eng->onMenuChanged();
        return ev::undefined();
    });

    menu.def("addItem", 2, [](Value, std::span<const Value> a) -> Value {
        auto* eng = hostEngine();
        if (!eng || a.size() < 2) return ev::fromBool(false);
        std::string parentId = ev::toUtf8(a[0]);
        engine::MenuBar::Item item = parseMenuItem(a[1]);
        int idx = a.size() > 2 ? static_cast<int>(ev::toDouble(a[2])) : -1;
        bool ok = eng->menuBar().addItem(parentId, std::move(item), idx);
        if (ok) eng->onMenuChanged();
        return ev::fromBool(ok);
    });

    menu.def("updateItem", 2, [](Value, std::span<const Value> a) -> Value {
        auto* eng = hostEngine();
        if (!eng || a.size() < 2) return ev::fromBool(false);
        std::string id = ev::toUtf8(a[0]);
        Value props = a[1];
        if (!ev::isObject(props)) return ev::fromBool(false);

        auto* item = eng->menuBar().find(id);
        if (!item) return ev::fromBool(false);

        Value lbl = ev::getProperty(props, "label");
        if (ev::isString(lbl)) item->label = ev::toUtf8(lbl);
        Value acc = ev::getProperty(props, "accel");
        if (ev::isString(acc)) item->accel = ev::toUtf8(acc);
        Value en = ev::getProperty(props, "enabled");
        if (!ev::isUndefined(en)) item->enabled = ev::toBool(en);
        Value hid = ev::getProperty(props, "hidden");
        if (!ev::isUndefined(hid)) item->hidden = ev::toBool(hid);
        Value chk = ev::getProperty(props, "checked");
        if (!ev::isUndefined(chk)) item->checked = ev::toBool(chk);

        eng->menuBar().dirty = true;
        eng->onMenuChanged();
        return ev::fromBool(true);
    });

    menu.def("removeItem", 1, [](Value, std::span<const Value> a) -> Value {
        auto* eng = hostEngine();
        if (!eng || a.empty()) return ev::fromBool(false);
        std::string id = ev::toUtf8(a[0]);
        bool ok = eng->menuBar().removeItem(id);
        if (ok) eng->onMenuChanged();
        return ev::fromBool(ok);
    });

    menu.def("on", 2, [](Value, std::span<const Value> a) -> Value {
        auto* eng = hostEngine();
        if (!eng || a.size() < 2 || !ev::isFunction(a[1])) return ev::undefined();
        std::string id = ev::toUtf8(a[0]);
        s_menuHandlers[id] = ev::Persistent(a[1]);
        eng->menuBar().on(id, [id]() {
            auto it = s_menuHandlers.find(id);
            if (it != s_menuHandlers.end() && ev::isFunction(it->second.get())) {
                ev::call(it->second.get(), ev::undefined(), {});
            }
        });
        return ev::undefined();
    });

    menu.def("getMenu", 0, [](Value, std::span<const Value>) -> Value {
        auto* eng = hostEngine();
        if (!eng) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        const auto& roots = eng->menuBar().roots;
        return hostArrayOf(roots.size(), [&roots](size_t i) -> Value {
            return menuItemToJS(roots[i]);
        });
    });

    return menu.get();
}

} // namespace bro::bronze_host
