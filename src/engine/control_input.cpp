// Agent-control input (docs/agent-control.md): pointer, wheel and keyboard
// as a real device would deliver them. Every event goes through
// Engine::injectDeviceInput — under DRM the libinput path, so global
// hotkeys, hover, the shell and client windows see it exactly as they see
// the machine's own mouse and keyboard.
//
// A command builds a timeline of events (a drag's motion every 8 ms, a key's
// press and release, the gap between typed characters) and a ticker plays it
// out over the frames that follow; the reply comes when the last event has
// been delivered. Coordinates are CSS px (client coordinates), or a CSS
// selector, which aims at the centre of the first match with a box.

#include "engine/control.h"
#include "engine/control_helpers.h"
#include "engine/engine.h"
#include "platform/evdev_keymap.h"
#include "util/json_out.h"
#include "util/time.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>

namespace bro::engine {

namespace {

// evdev KEY_* codes (linux/input-event-codes.h), by the names a key spec uses.
struct KeyName {
    const char* name;
    uint32_t code;
};
constexpr KeyName kKeys[] = {
    {"esc", 1}, {"escape", 1}, {"1", 2}, {"2", 3}, {"3", 4}, {"4", 5}, {"5", 6}, {"6", 7}, {"7", 8}, {"8", 9},
    {"9", 10}, {"0", 11}, {"minus", 12}, {"-", 12}, {"equal", 13}, {"=", 13}, {"backspace", 14}, {"tab", 15},
    {"q", 16}, {"w", 17}, {"e", 18}, {"r", 19}, {"t", 20}, {"y", 21}, {"u", 22}, {"i", 23}, {"o", 24}, {"p", 25},
    {"leftbrace", 26}, {"[", 26}, {"rightbrace", 27}, {"]", 27}, {"enter", 28}, {"return", 28}, {"ctrl", 29},
    {"control", 29}, {"leftctrl", 29}, {"a", 30}, {"s", 31}, {"d", 32}, {"f", 33}, {"g", 34}, {"h", 35}, {"j", 36},
    {"k", 37}, {"l", 38}, {"semicolon", 39}, {";", 39}, {"apostrophe", 40}, {"'", 40}, {"grave", 41}, {"`", 41},
    {"shift", 42}, {"leftshift", 42}, {"backslash", 43}, {"\\", 43}, {"z", 44}, {"x", 45}, {"c", 46}, {"v", 47},
    {"b", 48}, {"n", 49}, {"m", 50}, {"comma", 51}, {",", 51}, {"dot", 52}, {"period", 52}, {".", 52},
    {"slash", 53}, {"/", 53}, {"rightshift", 54}, {"alt", 56}, {"leftalt", 56}, {"space", 57}, {"capslock", 58},
    {"f1", 59}, {"f2", 60}, {"f3", 61}, {"f4", 62}, {"f5", 63}, {"f6", 64}, {"f7", 65}, {"f8", 66}, {"f9", 67},
    {"f10", 68}, {"f11", 87}, {"f12", 88}, {"rightctrl", 97}, {"print", 99}, {"sysrq", 99}, {"altgr", 100},
    {"rightalt", 100}, {"home", 102}, {"up", 103}, {"pageup", 104}, {"left", 105}, {"right", 106}, {"end", 107},
    {"down", 108}, {"pagedown", 109}, {"insert", 110}, {"delete", 111}, {"del", 111}, {"mute", 113},
    {"volumedown", 114}, {"volumeup", 115}, {"power", 116}, {"pause", 119}, {"super", 125}, {"meta", 125},
    {"win", 125}, {"logo", 125}, {"leftmeta", 125}, {"rightmeta", 126}, {"menu", 127}, {"compose", 127},
    {"nextsong", 163}, {"playpause", 164}, {"previoussong", 165}, {"brightnessdown", 224}, {"brightnessup", 225},
};

bool keyCode(std::string name, uint32_t& code) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
    for (const auto& k : kKeys)
        if (name == k.name) return code = k.code, true;
    return false;
}

// A character on a US layout: its key and whether it needs shift.
bool charKey(char c, uint32_t& code, bool& shift) {
    static const char* kShifted = "~!@#$%^&*()_+{}|:\"<>?";
    static const char* kPlain = "`1234567890-=[]\\;',./";
    shift = false;
    if (c >= 'A' && c <= 'Z') {
        shift = true;
        c = static_cast<char>(c - 'A' + 'a');
    }
    if (c == ' ') return code = 57, true;
    if (c == '\n') return code = 28, true;
    if (c == '\t') return code = 15, true;
    if (const char* p = std::strchr(kShifted, c); p && c) {
        shift = true;
        c = kPlain[p - kShifted];
    }
    const char s[2] = {c, 0};
    return keyCode(s, code);
}

uint32_t buttonCode(const std::string& b) {
    if (b == "right" || b == "3") return platform::kEvdevBtnRight;
    if (b == "middle" || b == "2") return platform::kEvdevBtnMiddle;
    if (b == "back" || b == "side") return platform::kEvdevBtnSide;
    if (b == "forward" || b == "extra") return platform::kEvdevBtnExtra;
    return platform::kEvdevBtnLeft;
}

// A timeline of device events, played out by a ticker.
struct InputPlayer {
    struct Step {
        double atMs;  // from the start
        Engine::DeviceInput in;
        bool isMove = false;
        float cssX = 0, cssY = 0;
    };
    std::deque<Step> steps;
    double t = 0;     // where the next step goes
    float px = 0, py = 0;  // the pointer, CSS px, as the timeline leaves it

    void move(float x, float y) {
        Step s;
        s.atMs = t;
        s.in.kind = Engine::DeviceInput::Kind::Motion;
        s.isMove = true;
        s.cssX = x;
        s.cssY = y;
        steps.push_back(s);
        px = x;
        py = y;
    }
    void glide(float x, float y, double ms) {
        const int n = std::max(1, static_cast<int>(ms / 8.0));
        const float x0 = px, y0 = py;
        for (int i = 1; i <= n; ++i) {
            const float f = static_cast<float>(i) / n;
            // Ease in-out, as a hand moves.
            const float e = f * f * (3 - 2 * f);
            t += ms / n;
            move(x0 + (x - x0) * e, y0 + (y - y0) * e);
        }
    }
    void button(uint32_t code, bool pressed) {
        Step s;
        s.atMs = t;
        s.in.kind = Engine::DeviceInput::Kind::Button;
        s.in.code = code;
        s.in.pressed = pressed;
        steps.push_back(s);
    }
    void key(uint32_t code, bool pressed) {
        Step s;
        s.atMs = t;
        s.in.kind = Engine::DeviceInput::Kind::Key;
        s.in.code = code;
        s.in.pressed = pressed;
        steps.push_back(s);
    }
    void wheel(int dx120, int dy120) {
        Step s;
        s.atMs = t;
        s.in.kind = Engine::DeviceInput::Kind::Wheel;
        s.in.wheelX = dx120;
        s.in.wheelY = dy120;
        steps.push_back(s);
    }
    void wait(double ms) { t += ms; }
};

void play(ControlServer& s, const ControlCallPtr& call, std::shared_ptr<InputPlayer> p) {
    Engine& e = s.engine();
    ControlServer* server = &s;
    const double start = s.clockMs();
    auto delivered = std::make_shared<int>(0);
    auto tick = [&e, server, call, p, start, delivered]() {
        const double now = server->clockMs() - start;
        const float sx = e.viewportWidth() > 0 ? static_cast<float>(e.framePixelWidth()) / e.viewportWidth() : 1.0f;
        const float sy = e.viewportHeight() > 0 ? static_cast<float>(e.framePixelHeight()) / e.viewportHeight() : 1.0f;
        while (!p->steps.empty() && p->steps.front().atMs <= now) {
            InputPlayer::Step st = p->steps.front();
            p->steps.pop_front();
            if (st.isMove) {
                st.in.x = st.cssX * sx;
                st.in.y = st.cssY * sy;
            }
            e.injectDeviceInput(st.in);
            ++*delivered;
        }
        if (!p->steps.empty()) return false;
        util::JsonOut j;
        j.beginObject().key("events").integer(*delivered).key("ms").number(server->clockMs() - start, 1);
        j.key("pointer").beginObject().key("x").number(e.pointerX(), 1).key("y").number(e.pointerY(), 1);
        j.endObject().endObject();
        call->ok(j.take());
        return true;
    };
    // The first events go now, in this frame, rather than one frame on.
    if (!tick()) s.addTicker(tick);
}

std::shared_ptr<InputPlayer> newPlayer(Engine& e) {
    auto p = std::make_shared<InputPlayer>();
    p->px = e.pointerX();
    p->py = e.pointerY();
    return p;
}

void cmdMove(ControlServer& s, const ControlCallPtr& call) {
    auto args = call->positional();
    float x, y;
    std::string why;
    if (!controlResolvePoint(s.engine(), args, 0, x, y, nullptr, &why)) return call->fail(why);
    auto p = newPlayer(s.engine());
    const double ms = call->number("ms", 0);
    if (ms > 0) p->glide(x, y, ms);
    else p->move(x, y);
    play(s, call, p);
}

void cmdClick(ControlServer& s, const ControlCallPtr& call) {
    auto args = call->positional();
    auto p = newPlayer(s.engine());
    if (!args.empty()) {
        float x, y;
        std::string why;
        if (!controlResolvePoint(s.engine(), args, 0, x, y, nullptr, &why)) return call->fail(why);
        const double ms = call->number("ms", 0);
        if (ms > 0) p->glide(x, y, ms);
        else p->move(x, y);
        p->wait(16);
    }
    const uint32_t btn = buttonCode(call->option("button", "left"));
    const int count = std::clamp(static_cast<int>(call->number("count", 1)), 1, 3);
    const double hold = call->number("hold", 40);
    for (int i = 0; i < count; ++i) {
        p->button(btn, true);
        p->wait(hold);
        p->button(btn, false);
        p->wait(60);
    }
    play(s, call, p);
}

void cmdButton(ControlServer& s, const ControlCallPtr& call, bool pressed) {
    auto p = newPlayer(s.engine());
    auto args = call->positional();
    if (!args.empty()) {
        float x, y;
        std::string why;
        if (!controlResolvePoint(s.engine(), args, 0, x, y, nullptr, &why)) return call->fail(why);
        p->move(x, y);
    }
    p->button(buttonCode(call->option("button", "left")), pressed);
    play(s, call, p);
}

void cmdDrag(ControlServer& s, const ControlCallPtr& call) {
    auto args = call->positional();
    float x0, y0, x1, y1;
    size_t used = 0;
    std::string why;
    if (!controlResolvePoint(s.engine(), args, 0, x0, y0, &used, &why)) return call->fail(why);
    if (!controlResolvePoint(s.engine(), args, used, x1, y1, nullptr, &why)) return call->fail(why);
    auto p = newPlayer(s.engine());
    const uint32_t btn = buttonCode(call->option("button", "left"));
    p->move(x0, y0);
    p->wait(30);
    p->button(btn, true);
    p->wait(50);
    p->glide(x1, y1, std::max(16.0, call->number("ms", 400)));
    p->wait(call->number("settle", 50));
    p->button(btn, false);
    play(s, call, p);
}

void cmdWheel(ControlServer& s, const ControlCallPtr& call) {
    auto args = call->positional();
    if (args.empty()) return call->fail("usage: wheel <dy detents, + is down> [dx] [--at=target] [--steps=N]");
    const double dy = std::atof(args[0].c_str());
    const double dx = args.size() > 1 ? std::atof(args[1].c_str()) : 0.0;
    auto p = newPlayer(s.engine());
    const std::string at = call->option("at");
    if (!at.empty()) {
        float x, y;
        std::string why;
        if (!controlResolvePoint(s.engine(), {at}, 0, x, y, nullptr, &why)) return call->fail(why);
        p->move(x, y);
        p->wait(16);
    }
    // One detent per step, a notch every 16 ms, as a wheel turned by hand.
    const int steps = std::max(1, static_cast<int>(call->number("steps", std::max(std::fabs(dy), std::fabs(dx)))));
    for (int i = 0; i < steps; ++i) {
        p->wheel(static_cast<int>(std::lround(dx * 120 / steps)), static_cast<int>(std::lround(dy * 120 / steps)));
        p->wait(16);
    }
    play(s, call, p);
}

// "ctrl+shift+t" -> modifiers then the key, released in reverse.
bool addCombo(InputPlayer& p, const std::string& combo, double hold, std::string* why) {
    std::vector<uint32_t> codes;
    size_t i = 0;
    while (i <= combo.size()) {
        size_t j = combo.find('+', i);
        // A bare "+" key: "ctrl++".
        if (j == i && i < combo.size()) j = combo.find('+', i + 1);
        if (j == std::string::npos) j = combo.size();
        std::string part = combo.substr(i, j - i);
        if (part.empty()) break;
        uint32_t code = 0;
        bool shift = false;
        if (keyCode(part, code)) {
            codes.push_back(code);
        } else if (part.size() == 1 && charKey(part[0], code, shift)) {
            if (shift) codes.push_back(42);
            codes.push_back(code);
        } else {
            if (why) *why = "unknown key '" + part + "' (names: ctrl shift alt super, a-z, 0-9, f1-f12, enter, "
                            "escape, tab, space, backspace, delete, up down left right, home end pageup pagedown, "
                            "volumeup volumedown mute, brightnessup brightnessdown, print ...)";
            return false;
        }
        i = j + 1;
    }
    if (codes.empty()) {
        if (why) *why = "empty key combination";
        return false;
    }
    for (uint32_t c : codes) {
        p.key(c, true);
        p.wait(8);
    }
    p.wait(hold);
    for (auto it = codes.rbegin(); it != codes.rend(); ++it) {
        p.key(*it, false);
        p.wait(8);
    }
    return true;
}

void cmdKey(ControlServer& s, const ControlCallPtr& call) {
    auto args = call->positional();
    if (args.empty()) return call->fail("usage: key <combo> [combo...]  e.g. key super+space, key escape");
    auto p = newPlayer(s.engine());
    const double hold = call->number("hold", 30);
    for (const auto& combo : args) {
        std::string why;
        if (!addCombo(*p, combo, hold, &why)) return call->fail(why);
        p->wait(call->number("gap", 40));
    }
    play(s, call, p);
}

void cmdType(ControlServer& s, const ControlCallPtr& call) {
    auto args = call->positional();
    if (args.empty()) return call->fail("usage: type <text> [--delay=12]");
    std::string text;
    for (size_t i = 0; i < args.size(); ++i) text += (i ? " " : "") + args[i];
    auto p = newPlayer(s.engine());
    const double delay = call->number("delay", 12);
    for (char c : text) {
        uint32_t code = 0;
        bool shift = false;
        if (static_cast<unsigned char>(c) >= 0x80 || !charKey(c, code, shift))
            return call->fail(std::string("cannot type '") + c + "' (US layout, ASCII only)");
        if (shift) p->key(42, true);
        p->key(code, true);
        p->wait(4);
        p->key(code, false);
        if (shift) p->key(42, false);
        p->wait(delay);
    }
    play(s, call, p);
}

}  // namespace

void registerControlInputCommands(ControlServer& s) {
    s.registerCommand("move", "<x y | x,y | selector> [--ms=0]  move the pointer (gliding over --ms)",
                      [&s](const ControlCallPtr& c) { cmdMove(s, c); });
    s.registerCommand("click",
                      "[x y | x,y | selector] [--button=left|right|middle] [--count=1] [--hold=40] [--ms=0]  "
                      "move there and click",
                      [&s](const ControlCallPtr& c) { cmdClick(s, c); });
    s.registerCommand("down", "[target] [--button=left]  press a button (and hold it)",
                      [&s](const ControlCallPtr& c) { cmdButton(s, c, true); });
    s.registerCommand("up", "[target] [--button=left]  release a button",
                      [&s](const ControlCallPtr& c) { cmdButton(s, c, false); });
    s.registerCommand("drag", "<from> <to> [--ms=400] [--button=left]  press at one point, glide, release",
                      [&s](const ControlCallPtr& c) { cmdDrag(s, c); });
    s.registerCommand("wheel", "<dy> [dx] [--at=target] [--steps=N]  scroll, in wheel detents (+dy is down)",
                      [&s](const ControlCallPtr& c) { cmdWheel(s, c); });
    s.registerCommand("key", "<combo>...  [--hold=30] [--gap=40]  press keys: super+space, ctrl+shift+t, escape",
                      [&s](const ControlCallPtr& c) { cmdKey(s, c); });
    s.registerCommand("type", "<text>  [--delay=12]  type ASCII text (US layout)",
                      [&s](const ControlCallPtr& c) { cmdType(s, c); });
}

}  // namespace bro::engine
