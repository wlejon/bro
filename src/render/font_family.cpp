#include "render/font_family.h"

#include <sstream>
#include <string>

namespace bro::render {

namespace {

// CSS generic family name -> real platform font name (must stay in sync
// across every renderer; this is the one table).
const char* resolveGenericFamily(const std::string& name) {
#ifdef _WIN32
    if (name == "sans-serif")  return "Arial";
    if (name == "serif")       return "Times New Roman";
    if (name == "monospace")   return "Consolas";
    if (name == "cursive")     return "Comic Sans MS";
    if (name == "fantasy")     return "Impact";
    if (name == "system-ui")   return "Segoe UI";
#elif defined(__APPLE__)
    if (name == "sans-serif")  return "Arial";
    if (name == "serif")       return "Times New Roman";
    if (name == "monospace")   return "Menlo";
    if (name == "cursive")     return "Apple Chancery";
    if (name == "fantasy")     return "Papyrus";
    if (name == "system-ui")   return "Helvetica Neue";
#else
    if (name == "sans-serif")  return "Liberation Sans";
    if (name == "serif")       return "Liberation Serif";
    if (name == "monospace")   return "Liberation Mono";
    if (name == "cursive")     return "DejaVu Sans";
    if (name == "fantasy")     return "DejaVu Sans";
    if (name == "system-ui")   return "Liberation Sans";
#endif
    return nullptr;
}

} // namespace

sk_sp<SkTypeface> resolveFontFamilyList(std::string_view cssFamily,
                                         SkFontStyle style,
                                         SkFontMgr* mgr) {
    if (!mgr) return nullptr;

    // CSS font-family is comma-separated — try each name in order.
    std::istringstream stream{std::string(cssFamily)};
    std::string name;
    while (std::getline(stream, name, ',')) {
        while (!name.empty() && (name.front() == ' ' || name.front() == '\'' || name.front() == '"')) name.erase(name.begin());
        while (!name.empty() && (name.back() == ' ' || name.back() == '\'' || name.back() == '"')) name.pop_back();
        if (name.empty()) continue;
        if (const char* resolved = resolveGenericFamily(name)) {
            if (auto tf = sk_sp<SkTypeface>(mgr->matchFamilyStyle(resolved, style))) return tf;
        }
        if (auto tf = sk_sp<SkTypeface>(mgr->matchFamilyStyle(name.c_str(), style))) return tf;
    }
    return sk_sp<SkTypeface>(mgr->matchFamilyStyle(nullptr, style));
}

} // namespace bro::render
