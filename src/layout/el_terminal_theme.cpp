// ElTerminal's options and colours.
//
// The palette (the 16 ANSI colours and the rest of the 256, foreground,
// background, cursor) and the overlay colours (selection, search matches)
// come from three places, the later winning slot by slot:
//   1. bropty's standard palette;
//   2. CSS: an element given a background-color uses it and its `color` as
//      the default background / foreground, and the custom properties
//      --terminal-foreground, --terminal-background, --terminal-cursor,
//      --terminal-selection, --terminal-match, --terminal-current-match and
//      --terminal-color-0 ... --terminal-color-255 set their slots;
//   3. what script set (setTheme / the `theme` property).
// The result is the session's base palette: what the program's OSC 4 / 10 /
// 11 / 12 resets return to.

#include "layout/el_terminal_impl.h"

#include "css/color.h"
#include "dom/element.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace bro::layout {

namespace {

std::string trim(std::string s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool parse(const std::string& v, htmlayout::css::Color& out) {
    const std::string t = trim(v);
    return !t.empty() && htmlayout::css::tryParseColor(t, out);
}

bropty::Rgb rgbOf(const htmlayout::css::Color& c) { return bropty::Rgb{c.r, c.g, c.b}; }
bromath::Color overlayOf(const htmlayout::css::Color& c) {
    return bromath::Color{float(c.r) / 255.0f, float(c.g) / 255.0f, float(c.b) / 255.0f, float(c.a) / 255.0f};
}

std::string hex(bropty::Rgb c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

std::string rgba(const bromath::Color& c) {
    auto u8 = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    char buf[48];
    std::snprintf(buf, sizeof buf, "rgba(%d, %d, %d, %.3g)", u8(c.r), u8(c.g), u8(c.b), double(c.a));
    return buf;
}

// One slot: the script's value, else the custom property, else `fallback`.
struct Sources {
    const htmlayout::css::ComputedStyle& style;
    const ElTerminal::Theme& script;
    bool slot(const std::string& scriptValue, const char* prop, htmlayout::css::Color& out) const {
        if (parse(scriptValue, out)) return true;
        if (const std::string* v = style.customProperty(prop)) return parse(*v, out);
        return false;
    }
};

} // namespace

void ElTerminal::refreshTheme() {
    Impl& m = *impl_;
    if (!elem_) return;
    const auto& style = elem_->computedStyle();
    const Sources src{style, scriptTheme_};

    bropty::Palette pal = bropty::Palette::standard();
    htmlayout::css::Color c;
    // The element's own colours, when it was given a background.
    if (auto it = style.find("background-color"); it != style.end() && parse(it->second, c) && c.a > 0) {
        pal.background = rgbOf(c);
        if (auto ct = style.find("color"); ct != style.end() && parse(ct->second, c)) pal.foreground = rgbOf(c);
    }
    if (src.slot(scriptTheme_.foreground, "--terminal-foreground", c)) pal.foreground = rgbOf(c);
    if (src.slot(scriptTheme_.background, "--terminal-background", c)) pal.background = rgbOf(c);
    pal.cursor = pal.foreground;
    if (src.slot(scriptTheme_.cursor, "--terminal-cursor", c)) pal.cursor = rgbOf(c);
    // --terminal-color-N for all 256 indexes: the ones the element sees are
    // found by one pass over its custom properties (inherited, then its own),
    // not 256 lookups a frame.
    std::array<const std::string*, 256> cssSlots{};
    auto collect = [&cssSlots](const htmlayout::css::StyleMap& vars) {
        constexpr std::string_view kPrefix = "--terminal-color-";
        for (const auto& [name, value] : vars) {
            if (name.size() <= kPrefix.size() || name.compare(0, kPrefix.size(), kPrefix) != 0) continue;
            int n = 0;
            bool ok = name.size() - kPrefix.size() <= 3;
            for (size_t i = kPrefix.size(); ok && i < name.size(); ++i) {
                ok = name[i] >= '0' && name[i] <= '9';
                n = n * 10 + (name[i] - '0');
            }
            if (ok && n < 256) cssSlots[size_t(n)] = &value;
        }
    };
    if (style.inheritedVars) collect(*style.inheritedVars);
    collect(style);
    for (size_t i = 0; i < 256; ++i) {
        if (parse(scriptTheme_.ansi[i], c) || (cssSlots[i] && parse(*cssSlots[i], c))) pal.colors[i] = rgbOf(c);
    }
    terminal::HighlightColors hl;
    if (src.slot(scriptTheme_.selection, "--terminal-selection", c)) hl.selection = overlayOf(c);
    if (src.slot(scriptTheme_.match, "--terminal-match", c)) hl.match = overlayOf(c);
    if (src.slot(scriptTheme_.currentMatch, "--terminal-current-match", c)) hl.currentMatch = overlayOf(c);

    // What changed is told by the raw values; the "#rrggbb" strings are only
    // built when something did.
    std::string key;
    key.reserve(sizeof(bropty::Rgb) * 259 + sizeof(hl));
    auto addBytes = [&key](const void* p, size_t n) { key.append(static_cast<const char*>(p), n); };
    addBytes(pal.colors.data(), sizeof(pal.colors));
    addBytes(&pal.foreground, sizeof(pal.foreground));
    addBytes(&pal.background, sizeof(pal.background));
    addBytes(&pal.cursor, sizeof(pal.cursor));
    const size_t paletteBytes = key.size();
    addBytes(&hl, sizeof(hl));
    terminal::ColorPolicy policy;
    policy.boldIsBright = options_.boldIsBright;
    policy.minimumContrast = options_.minimumContrast;

    if (key != m.paletteKey) {
        // The palette reaches the paint through the session's next frame.
        const bool paletteChanged = m.paletteKey.size() != key.size() ||
                                    m.paletteKey.compare(0, paletteBytes, key, 0, paletteBytes) != 0;
        m.paletteKey = std::move(key);
        Theme resolved;
        resolved.foreground = hex(pal.foreground);
        resolved.background = hex(pal.background);
        resolved.cursor = hex(pal.cursor);
        resolved.selection = rgba(hl.selection);
        resolved.match = rgba(hl.match);
        resolved.currentMatch = rgba(hl.currentMatch);
        for (size_t i = 0; i < 256; ++i) resolved.ansi[i] = hex(pal.colors[i]);
        m.resolved = std::move(resolved);
        m.highlights = hl;
        if (paletteChanged) m.session->setBasePalette(pal);
        m.layerDirty = true;
    }
    if (!(policy == m.colors)) {
        m.colors = policy;
        m.layerDirty = true;
    }
}

ElTerminal::Theme ElTerminal::theme() const { return impl_->resolved; }

void ElTerminal::setTheme(const Theme& t) {
    scriptTheme_ = t;
    refreshTheme();
}

void ElTerminal::setOptions(const Options& o) {
    Impl& m = *impl_;
    {
        // refreshFont() reads `ligatures` on the layout thread too.
        std::lock_guard<std::mutex> g(m.fontMu);
        options_ = o;
        options_.minimumContrast = std::clamp(o.minimumContrast, 1.0f, 21.0f);
        options_.wheelLines = std::clamp(o.wheelLines, 1, 100);
    }
    terminal::ClipboardPolicy p = terminal::ClipboardPolicy::WriteOnly;
    if (o.clipboard == "deny") p = terminal::ClipboardPolicy::Deny;
    else if (o.clipboard == "read-write") p = terminal::ClipboardPolicy::ReadWrite;
    m.session->setClipboardPolicy(p);
    m.session->setScrollOnInput(o.scrollOnInput);
    refreshFont();   // ligatures change the cell width
    refreshTheme();  // the colour policy
    m.layerDirty = true;
}

} // namespace bro::layout
