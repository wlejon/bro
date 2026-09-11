#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::engine {

// Tree-backed data model for the standard app menu bar. Rendered by
// system/menu.html; dispatched back through Engine::triggerMenuAction().
//
// IDs that start with "__system." are reserved for engine-handled actions
// (preferences, quit, about). Everything else is routed to registered handlers.
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

    // Hidden by default — apps that want a menu bar opt in with
    // menuBar().visible = true at startup.
    // The flag is honored identically in every display mode, including
    // bro-headless: a shown menu reserves its contentTop() inset and
    // renders in screenshots, so headless testing exercises the same
    // viewport geometry as the windowed app.
    bool visible = false;
    // Height in CSS pixels. Also drives Engine::contentInsets(). Read by
    // system/menu.html via __bro.menu.getHeight() to size #menu-bar — not
    // hardcoded there.
    int height = 28;
    std::vector<Item> roots;

    // Mutations — all set dirty = true.
    void clear();
    void setRoots(std::vector<Item> items);
    Item* find(const std::string& id);
    bool addItem(const std::string& parentId, Item item, int index = -1);
    bool updateItem(const std::string& id, const Item& props);
    bool removeItem(const std::string& id);

    // Handler registry.
    void on(const std::string& id, std::function<void()> fn);
    bool hasHandler(const std::string& id) const;
    // Calls the handler; returns true if one was registered.
    bool triggerHandler(const std::string& id);
    void releaseHandlers();

    // Serialize the visible tree for the system panel context.
    std::string toJSON() const;

    // Set by mutations; consumed and cleared by the engine after re-render.
    bool dirty = true;

private:
    static Item* findIn(std::vector<Item>& items, const std::string& id);
    static void appendJSON(const Item& item, std::string& out);

    std::unordered_map<std::string, std::function<void()>> handlers_;
};

} // namespace bro::engine
