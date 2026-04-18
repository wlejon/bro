#pragma once

#include <string>
#include <vector>
#include <unordered_map>

extern "C" {
#include "quickjs.h"
}

namespace bro::engine {

// Tree-backed data model for the standard app menu bar. Rendered by
// system/menu.html; mutated from app JS via bro.menu.*; dispatched back
// through Engine::triggerMenuAction().
//
// IDs that start with "__system." are reserved for engine-handled actions
// (preferences, quit, about). Everything else is routed to app JS handlers
// registered via bro.menu.on(id, fn).
class MenuBar {
public:
    struct Item {
        std::string id;
        std::string label;
        std::string accel;
        bool separator = false;
        bool enabled = true;
        bool hidden = false;
        bool checked = false;
        std::vector<Item> children;
    };

    bool visible = true;
    // Height in CSS pixels. Must match #menu-bar height in system/menu.html.
    int height = 28;
    std::vector<Item> roots;

    // Mutations — all set dirty = true.
    void clear();
    void setRootsFromJS(JSContext* ctx, JSValueConst arr);
    Item* find(const std::string& id);
    bool addItem(const std::string& parentId, Item item, int index);
    bool updateItem(JSContext* ctx, const std::string& id, JSValueConst props);
    bool removeItem(const std::string& id);

    // JS handler registry. Stored values are duplicated and freed on clear().
    void on(JSContext* ctx, const std::string& id, JSValueConst fn);
    bool hasHandler(const std::string& id) const;
    // Calls the handler; returns true if one was registered.
    bool triggerHandler(const std::string& id);
    void releaseHandlers();

    // Serialize the visible tree for the system panel context.
    std::string toJSON() const;

    // Set by mutations; consumed and cleared by the engine after re-render.
    bool dirty = true;

private:
    static Item parseItem(JSContext* ctx, JSValueConst obj);
    static void parseChildren(JSContext* ctx, JSValueConst arr,
                              std::vector<Item>& out);
    static Item* findIn(std::vector<Item>& items, const std::string& id);
    static void appendJSON(const Item& item, std::string& out);

    struct Handler {
        JSContext* ctx = nullptr;
        JSValue fn = JS_UNDEFINED;
    };
    std::unordered_map<std::string, Handler> handlers_;
};

} // namespace bro::engine
