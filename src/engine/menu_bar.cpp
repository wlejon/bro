#include "engine/menu_bar.h"
#include "util/log.h"

#include <cstdio>

namespace bro::engine {

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

static void appendJSONString(const std::string& s, std::string& out) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void MenuBar::appendJSON(const Item& item, std::string& out) {
    out += '{';
    out += "\"id\":";       appendJSONString(item.id, out);
    out += ",\"label\":";   appendJSONString(item.label, out);
    out += ",\"accel\":";   appendJSONString(item.accel, out);
    out += ",\"separator\":"; out += item.separator ? "true" : "false";
    out += ",\"enabled\":";   out += item.enabled   ? "true" : "false";
    out += ",\"hidden\":";    out += item.hidden    ? "true" : "false";
    out += ",\"checked\":";   out += item.checked   ? "true" : "false";
    out += ",\"children\":[";
    for (size_t i = 0; i < item.children.size(); ++i) {
        if (i) out += ',';
        appendJSON(item.children[i], out);
    }
    out += "]}";
}

std::string MenuBar::toJSON() const {
    std::string out = "[";
    for (size_t i = 0; i < roots.size(); ++i) {
        if (i) out += ',';
        appendJSON(roots[i], out);
    }
    out += "]";
    return out;
}

// ---------------------------------------------------------------------------
// JS parsing
// ---------------------------------------------------------------------------

static std::string readString(JSContext* ctx, JSValueConst obj, const char* key) {
    std::string result;
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (s) { result = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
    return result;
}

static bool readBool(JSContext* ctx, JSValueConst obj, const char* key, bool def) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    bool result = def;
    if (JS_IsBool(v)) result = (JS_ToBool(ctx, v) != 0);
    JS_FreeValue(ctx, v);
    return result;
}

MenuBar::Item MenuBar::parseItem(JSContext* ctx, JSValueConst obj) {
    Item item;
    if (!JS_IsObject(obj)) return item;
    item.id        = readString(ctx, obj, "id");
    item.label     = readString(ctx, obj, "label");
    item.accel     = readString(ctx, obj, "accel");
    item.separator = readBool(ctx, obj, "separator", false);
    item.enabled   = readBool(ctx, obj, "enabled", true);
    item.hidden    = readBool(ctx, obj, "hidden", false);
    item.checked   = readBool(ctx, obj, "checked", false);

    JSValue children = JS_GetPropertyStr(ctx, obj, "items");
    if (JS_IsUndefined(children)) {
        JS_FreeValue(ctx, children);
        children = JS_GetPropertyStr(ctx, obj, "children");
    }
    if (JS_IsArray(children)) parseChildren(ctx, children, item.children);
    JS_FreeValue(ctx, children);
    return item;
}

void MenuBar::parseChildren(JSContext* ctx, JSValueConst arr,
                            std::vector<Item>& out) {
    uint32_t len = 0;
    JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
    JS_ToUint32(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);
    out.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        JSValue el = JS_GetPropertyUint32(ctx, arr, i);
        out.push_back(parseItem(ctx, el));
        JS_FreeValue(ctx, el);
    }
}

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------

void MenuBar::clear() {
    roots.clear();
    dirty = true;
}

void MenuBar::setRootsFromJS(JSContext* ctx, JSValueConst arr) {
    roots.clear();
    if (JS_IsArray(arr)) parseChildren(ctx, arr, roots);
    dirty = true;
}

MenuBar::Item* MenuBar::findIn(std::vector<Item>& items, const std::string& id) {
    for (auto& it : items) {
        if (it.id == id) return &it;
        if (!it.children.empty()) {
            if (auto* r = findIn(it.children, id)) return r;
        }
    }
    return nullptr;
}

MenuBar::Item* MenuBar::find(const std::string& id) {
    return findIn(roots, id);
}

bool MenuBar::addItem(const std::string& parentId, Item item, int index) {
    std::vector<Item>* dst = nullptr;
    if (parentId.empty()) {
        dst = &roots;
    } else {
        auto* parent = find(parentId);
        if (!parent) return false;
        dst = &parent->children;
    }
    if (index < 0 || index > static_cast<int>(dst->size())) {
        dst->push_back(std::move(item));
    } else {
        dst->insert(dst->begin() + index, std::move(item));
    }
    dirty = true;
    return true;
}

bool MenuBar::updateItem(JSContext* ctx, const std::string& id, JSValueConst props) {
    auto* item = find(id);
    if (!item || !JS_IsObject(props)) return false;

    JSValue v;
    v = JS_GetPropertyStr(ctx, props, "label");
    if (JS_IsString(v)) { const char* s = JS_ToCString(ctx, v); if (s) { item->label = s; JS_FreeCString(ctx, s); } }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, props, "accel");
    if (JS_IsString(v)) { const char* s = JS_ToCString(ctx, v); if (s) { item->accel = s; JS_FreeCString(ctx, s); } }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, props, "enabled");
    if (JS_IsBool(v)) item->enabled = (JS_ToBool(ctx, v) != 0);
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, props, "hidden");
    if (JS_IsBool(v)) item->hidden = (JS_ToBool(ctx, v) != 0);
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, props, "checked");
    if (JS_IsBool(v)) item->checked = (JS_ToBool(ctx, v) != 0);
    JS_FreeValue(ctx, v);

    dirty = true;
    return true;
}

static bool removeIn(std::vector<MenuBar::Item>& items, const std::string& id) {
    for (auto it = items.begin(); it != items.end(); ++it) {
        if (it->id == id) { items.erase(it); return true; }
        if (removeIn(it->children, id)) return true;
    }
    return false;
}

bool MenuBar::removeItem(const std::string& id) {
    bool ok = removeIn(roots, id);
    if (ok) dirty = true;
    return ok;
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

void MenuBar::on(JSContext* ctx, const std::string& id, JSValueConst fn) {
    if (!JS_IsFunction(ctx, fn)) return;
    auto it = handlers_.find(id);
    if (it != handlers_.end()) {
        JS_FreeValue(it->second.ctx, it->second.fn);
        handlers_.erase(it);
    }
    handlers_[id] = Handler{ ctx, JS_DupValue(ctx, fn) };
}

bool MenuBar::hasHandler(const std::string& id) const {
    return handlers_.find(id) != handlers_.end();
}

bool MenuBar::triggerHandler(const std::string& id) {
    auto it = handlers_.find(id);
    if (it == handlers_.end()) return false;
    JSContext* ctx = it->second.ctx;
    JSValue fn = JS_DupValue(ctx, it->second.fn);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue result = JS_Call(ctx, fn, global, 0, nullptr);
    if (JS_IsException(result)) {
        JSValue ex = JS_GetException(ctx);
        const char* s = JS_ToCString(ctx, ex);
        if (s) { LOG_ERROR("menu handler '%s' threw: %s", id.c_str(), s); JS_FreeCString(ctx, s); }
        JS_FreeValue(ctx, ex);
    }
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, global);
    return true;
}

void MenuBar::releaseHandlers() {
    for (auto& kv : handlers_) {
        JS_FreeValue(kv.second.ctx, kv.second.fn);
    }
    handlers_.clear();
}

} // namespace bro::engine
