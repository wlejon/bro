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
// Mutations
// ---------------------------------------------------------------------------

void MenuBar::clear() {
    roots.clear();
    dirty = true;
}

void MenuBar::setRoots(std::vector<Item> items) {
    roots = std::move(items);
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

static bool removeIn(std::vector<MenuBar::Item>& items, const std::string& id) {
    for (auto it = items.begin(); it != items.end(); ++it) {
        if (it->id == id) { items.erase(it); return true; }
        if (removeIn(it->children, id)) return true;
    }
    return false;
}

bool MenuBar::updateItem(const std::string& id, const Item& props) {
    auto* item = find(id);
    if (!item) return false;
    if (!props.label.empty()) item->label = props.label;
    if (!props.accel.empty()) item->accel = props.accel;
    item->enabled = props.enabled;
    item->hidden = props.hidden;
    item->checked = props.checked;
    dirty = true;
    return true;
}

bool MenuBar::removeItem(const std::string& id) {
    bool ok = removeIn(roots, id);
    if (ok) dirty = true;
    return ok;
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

void MenuBar::on(const std::string& id, std::function<void()> fn) {
    handlers_[id] = std::move(fn);
}

bool MenuBar::hasHandler(const std::string& id) const {
    return handlers_.find(id) != handlers_.end();
}

bool MenuBar::triggerHandler(const std::string& id) {
    auto it = handlers_.find(id);
    if (it == handlers_.end() || !it->second) return false;
    it->second();
    return true;
}

void MenuBar::releaseHandlers() {
    handlers_.clear();
}

} // namespace bro::engine
