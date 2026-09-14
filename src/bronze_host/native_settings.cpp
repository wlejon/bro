// `__bro_native.settings` — behind bro.settings (docs/settings.md): the
// three-layer store's reads and writes, the action-binding surface, the
// polled action state, the display modes, and the change callback.
//
// TYPES CROSS AS TEXT, in both directions, because the store itself is text
// (a flat key → string file) and the engine types the keys it owns while
// applying them. A read answers the resolved text and bro_core.js types it
// by content — `true`/`false`, a number, JSON for an object the wrapper
// stored — which is the one rule that works for the engine's keys and an
// app's own alike. A write arrives already split by JS type (setBool,
// setNumber, setString) so the store's own stringification runs, the same
// one bro.json values go through.
//
// A LIST OF STRUCTS (the actions, the display modes, a category's values)
// crosses as JSON the engine side emits and the wrapper parses. A LIST OF
// STRINGS going IN (an action's keys) crosses as one newline-joined `str`: a
// binding string is a key value or a `mouse:`/`gamepad:` spelling, and none
// of those can contain a newline.
//
// THE CHANGE CALLBACK is a `dynamic` kept in a Persistent and called with
// (category, key) from the host task queue, posted by Engine's settings
// observer — the engine's own hook, which fires after it has applied the
// change (installSettingsObserver, below, says why it is posted).

#include "bronze_host/host_internal.h"
#include "bronze_host/host_natives.h"
#include "engine/engine.h"
#include "engine/settings.h"
#include "platform/sdl_window.h"

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

engine::Settings* store() {
    auto* eng = hostEngine();
    return eng ? eng->settings() : nullptr;
}

// ---- JSON out --------------------------------------------------------------

void jsonString(const std::string& s, std::string& out) {
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
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void jsonNumber(double v, std::string& out) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    out += buf;
}

void jsonBool(bool v, std::string& out) { out += v ? "true" : "false"; }

void jsonStringList(const std::vector<std::string>& items, std::string& out) {
    out += '[';
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += ',';
        jsonString(items[i], out);
    }
    out += ']';
}

void jsonActions(const std::vector<engine::ActionBinding>& actions, std::string& out) {
    out += '[';
    for (size_t i = 0; i < actions.size(); ++i) {
        if (i) out += ',';
        out += "{\"action\":";
        jsonString(actions[i].action, out);
        out += ",\"keys\":";
        jsonStringList(actions[i].keys, out);
        out += '}';
    }
    out += ']';
}

void jsonGraphics(const engine::GraphicsSettings& g, std::string& out) {
    out += "{\"width\":"; jsonNumber(g.width, out);
    out += ",\"height\":"; jsonNumber(g.height, out);
    out += ",\"fullscreen\":"; jsonBool(g.fullscreen, out);
    out += ",\"vsync\":"; jsonBool(g.vsync, out);
    out += ",\"resizable\":"; jsonBool(g.resizable, out);
    out += ",\"maxFrameIntervalMs\":"; jsonNumber(g.maxFrameIntervalMs, out);
    out += ",\"maxFps\":"; jsonNumber(g.maxFps, out);
    out += '}';
}

void jsonAudio(const engine::AudioSettings& a, std::string& out) {
    out += "{\"masterVolume\":"; jsonNumber(a.masterVolume, out);
    out += ",\"musicVolume\":"; jsonNumber(a.musicVolume, out);
    out += ",\"sfxVolume\":"; jsonNumber(a.sfxVolume, out);
    out += ",\"muted\":"; jsonBool(a.muted, out);
    out += '}';
}

void jsonInput(const engine::InputSettings& i, std::string& out) {
    out += "{\"scrollSpeed\":"; jsonNumber(i.scrollSpeed, out);
    out += ",\"doubleClickThresholdMs\":"; jsonNumber(i.doubleClickThresholdMs, out);
    out += ",\"doubleClickDistancePx\":"; jsonNumber(i.doubleClickDistancePx, out);
    out += ",\"overlayToggleKey\":"; jsonNumber(static_cast<double>(i.overlayToggleKey), out);
    out += '}';
}

void jsonAppearance(const engine::AppearanceSettings& a, std::string& out) {
    out += "{\"colorScheme\":";
    jsonString(a.colorScheme, out);
    out += '}';
}

// An app's own category: every custom key under `<category>.`, its text as
// stored. The wrapper types each value the way it types a single get.
void jsonCustomCategory(const engine::SettingsData& data, const std::string& category,
                        std::string& out) {
    const std::string prefix = category + ".";
    out += '{';
    bool first = true;
    for (const auto& [key, value] : data.custom) {
        if (key.compare(0, prefix.size(), prefix) != 0) continue;
        if (!first) out += ',';
        first = false;
        jsonString(key.substr(prefix.size()), out);
        out += ':';
        jsonString(value, out);
    }
    out += '}';
}

std::string categoryJson(const engine::SettingsData& data, const std::string& category) {
    std::string out;
    if (category == "graphics") jsonGraphics(data.graphics, out);
    else if (category == "audio") jsonAudio(data.audio, out);
    else if (category == "input") jsonInput(data.input, out);
    else if (category == "appearance") jsonAppearance(data.appearance, out);
    else if (!category.empty()) jsonCustomCategory(data, category, out);
    else {
        out += "{\"graphics\":"; jsonGraphics(data.graphics, out);
        out += ",\"audio\":"; jsonAudio(data.audio, out);
        out += ",\"input\":"; jsonInput(data.input, out);
        out += ",\"appearance\":"; jsonAppearance(data.appearance, out);
        out += '}';
    }
    return out;
}

std::vector<std::string> splitLines(const char* joined) {
    std::vector<std::string> keys;
    std::istringstream ss(joined);
    std::string k;
    while (std::getline(ss, k, '\n')) {
        if (!k.empty()) keys.push_back(k);
    }
    return keys;
}

// ---- the natives -----------------------------------------------------------

const char* get(const char* key) {
    auto* s = store();
    return natives::strResult(s ? s->getString(key) : std::string());
}

const char* getAllJson(const char* category) {
    auto* s = store();
    if (!s) return "null";
    return natives::strResult(categoryJson(s->current(), category));
}

// Defaults are the engine + bro.json layer, exposed for graphics only (the
// one category a settings panel offers "reset to defaults" against).
const char* getDefaultsJson(const char* category) {
    auto* s = store();
    if (!s) return "null";
    const std::string cat = category;
    std::string out;
    if (cat == "graphics") {
        jsonGraphics(s->graphicsDefaults(), out);
    } else if (cat.empty()) {
        out += "{\"graphics\":";
        jsonGraphics(s->graphicsDefaults(), out);
        out += '}';
    } else {
        out = "{}";
    }
    return natives::strResult(std::move(out));
}

void setString(const char* key, const char* value) { if (auto* s = store()) s->setUser(key, std::string(value)); }
void setNumber(const char* key, double value) { if (auto* s = store()) s->setUser(key, value); }
void setBool(const char* key, bool value) { if (auto* s = store()) s->setUser(key, value); }
void setDefaultString(const char* key, const char* value) { if (auto* s = store()) s->setDefault(key, std::string(value)); }
void setDefaultNumber(const char* key, double value) { if (auto* s = store()) s->setDefault(key, value); }
void setDefaultBool(const char* key, bool value) { if (auto* s = store()) s->setDefault(key, value); }

// "" resets everything, a name one category.
void reset(const char* category) {
    auto* s = store();
    if (!s) return;
    if (*category) s->resetCategory(category);
    else s->resetAll();
}

// deadzone < 0: none given, the store's default stands.
void defineAction(const char* action, const char* keysJoined, double deadzone) {
    auto* s = store();
    if (!s) return;
    s->defineAction(action, splitLines(keysJoined));
    if (deadzone >= 0.0) s->setActionDeadzone(action, static_cast<float>(deadzone));
}

void rebindAction(const char* action, const char* keysJoined) {
    if (auto* s = store()) s->rebindAction(action, splitLines(keysJoined));
}

void resetAction(const char* action) { if (auto* s = store()) s->resetAction(action); }
void resetAllActions() { if (auto* s = store()) s->resetAllActions(); }

const char* actionKeysJson(const char* action) {
    auto* s = store();
    std::string out;
    jsonStringList(s ? s->getKeysForAction(action) : std::vector<std::string>{}, out);
    return natives::strResult(std::move(out));
}

// "" for no action: the wrapper answers null, as documented.
const char* keyAction(const char* key) {
    auto* s = store();
    return natives::strResult(s ? s->getActionForKey(key) : std::string());
}

double actionStrength(const char* action) {
    auto* eng = hostEngine();
    return eng ? static_cast<double>(eng->actionStrength(action)) : 0.0;
}

bool isActionPressed(const char* action) {
    auto* eng = hostEngine();
    return eng && eng->actionPressed(action);
}

const char* actionsJson() {
    auto* s = store();
    std::string out;
    jsonActions(s ? s->getActions() : std::vector<engine::ActionBinding>{}, out);
    return natives::strResult(std::move(out));
}

const char* appActionsJson() {
    auto* s = store();
    std::string out;
    jsonActions(s ? s->getAppActions() : std::vector<engine::ActionBinding>{}, out);
    return natives::strResult(std::move(out));
}

const char* displayModesJson() {
    auto* eng = hostEngine();
    auto* win = eng ? eng->window() : nullptr;
    std::string out = "[";
    if (win) {
        auto modes = win->getDisplayModes();
        for (size_t i = 0; i < modes.size(); ++i) {
            if (i) out += ',';
            out += "{\"width\":"; jsonNumber(modes[i].width, out);
            out += ",\"height\":"; jsonNumber(modes[i].height, out);
            out += ",\"refreshRate\":"; jsonNumber(modes[i].refreshRate, out);
            out += '}';
        }
    }
    out += ']';
    return natives::strResult(std::move(out));
}

// ---- the change callback ---------------------------------------------------

// Heap-allocated and never freed, like every Persistent this layer keeps for
// the life of the process: a static destructor would release the slot
// against a runtime whose statics may already be gone.
ev::Persistent* g_onChange = nullptr;

void onChange(uint64_t fnBits) {
    if (!g_onChange) g_onChange = new ev::Persistent();
    g_onChange->set(ev::fromBits(fnBits));
}

}  // namespace

// The engine's observer fires INSIDE the setUser that made the change — which
// is inside the compiled program's own call to setNumber/setString/setBool.
// The listener is not run there: it is posted to the host task queue and
// delivered at the top of the next frame seam, the same place an image's
// load event is delivered (host_internal.h, postHostTask), so compiled code
// re-enters only at a point the host owns.
void installSettingsObserver(engine::Engine& engine) {
    engine.setSettingsObserver([](const std::string& category, const std::string& key) {
        if (!g_onChange) return;
        postHostTask([category, key]() {
            if (!g_onChange || !ev::isFunction(g_onChange->get())) return;
            // Each argument in its own rooted slot before the call: fromUtf8
            // allocates, and the second allocation may move the first.
            ev::Persistent cat{ev::fromUtf8(category)};
            ev::Persistent k{ev::fromUtf8(key)};
            Value args[2] = {cat.get(), k.get()};
            ev::CallResult r = ev::call(g_onChange->get(), ev::undefined(), args);
            if (r.thrown) reportBronzeError("bro.settings.onChange", r.value);
        });
    });
}

bool registerSettingsNatives(std::string* error) {
    using namespace natives;
    auto p = [](auto f) { return reinterpret_cast<void*>(f); };
    return fn("__bro_native.settings.get", p(&get), "str", {"str"}, error) &&
           fn("__bro_native.settings.getAllJson", p(&getAllJson), "str", {"str"}, error) &&
           fn("__bro_native.settings.getDefaultsJson", p(&getDefaultsJson), "str", {"str"}, error) &&
           fn("__bro_native.settings.setString", p(&setString), "void", {"str", "str"}, error) &&
           fn("__bro_native.settings.setNumber", p(&setNumber), "void", {"str", "f64"}, error) &&
           fn("__bro_native.settings.setBool", p(&setBool), "void", {"str", "bool"}, error) &&
           fn("__bro_native.settings.setDefaultString", p(&setDefaultString), "void", {"str", "str"}, error) &&
           fn("__bro_native.settings.setDefaultNumber", p(&setDefaultNumber), "void", {"str", "f64"}, error) &&
           fn("__bro_native.settings.setDefaultBool", p(&setDefaultBool), "void", {"str", "bool"}, error) &&
           fn("__bro_native.settings.reset", p(&reset), "void", {"str"}, error) &&
           fn("__bro_native.settings.defineAction", p(&defineAction), "void", {"str", "str", "f64"}, error) &&
           fn("__bro_native.settings.rebindAction", p(&rebindAction), "void", {"str", "str"}, error) &&
           fn("__bro_native.settings.resetAction", p(&resetAction), "void", {"str"}, error) &&
           fn("__bro_native.settings.resetAllActions", p(&resetAllActions), "void", {}, error) &&
           fn("__bro_native.settings.actionKeysJson", p(&actionKeysJson), "str", {"str"}, error) &&
           fn("__bro_native.settings.keyAction", p(&keyAction), "str", {"str"}, error) &&
           fn("__bro_native.settings.actionStrength", p(&actionStrength), "f64", {"str"}, error) &&
           fn("__bro_native.settings.isActionPressed", p(&isActionPressed), "bool", {"str"}, error) &&
           fn("__bro_native.settings.actionsJson", p(&actionsJson), "str", {}, error) &&
           fn("__bro_native.settings.appActionsJson", p(&appActionsJson), "str", {}, error) &&
           fn("__bro_native.settings.displayModesJson", p(&displayModesJson), "str", {}, error) &&
           fn("__bro_native.settings.onChange", p(&onChange), "void", {"dynamic"}, error);
}

}  // namespace bro::bronze_host
