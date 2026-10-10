#include "render/font_family.h"
#include "render/system_font_mgr.h"

#include <chrono>
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

namespace {

bool equalsIgnoreAsciiCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// Rank of a face's weight for a desired weight under CSS Fonts 4 §5.2
// (lower is better). Three bands, each ordered by distance.
int weightRank(int desired, int face) {
    constexpr int kBand = 10000;
    if (desired >= 400 && desired <= 500) {
        if (face >= desired && face <= 500) return face - desired;
        if (face < desired) return kBand + (desired - face);
        return 2 * kBand + (face - 500);
    }
    if (desired < 400) {
        if (face <= desired) return desired - face;
        return kBand + (face - desired);
    }
    if (face >= desired) return face - desired;
    return kBand + (desired - face);
}

} // namespace

CustomFontMatch matchCustomFontFace(const std::vector<CustomFontFace>& faces,
                                    std::string_view family,
                                    int weight, bool italic) {
    CustomFontMatch best;
    bool bestStyleMatches = false;
    int bestRank = 0;
    for (const auto& f : faces) {
        if (!f.typeface || !equalsIgnoreAsciiCase(f.family, family)) continue;
        const bool styleMatches = f.italic == italic;
        const int rank = weightRank(weight, f.weight);
        // Style narrows first: a face of the wanted style beats any weight
        // of the other style.
        if (!best.face || (styleMatches && !bestStyleMatches) ||
            (styleMatches == bestStyleMatches && rank < bestRank)) {
            best.face = &f;
            bestStyleMatches = styleMatches;
            bestRank = rank;
        }
    }
    if (best.face) {
        best.syntheticBold = weight >= 600 && best.face->weight < 600;
        best.syntheticItalic = italic && !best.face->italic;
    }
    return best;
}

void applyFontSynthesis(SkFont& font, const CustomFontMatch& match) {
    if (match.syntheticBold) font.setEmbolden(true);
    // Skia's (and Blink's) synthetic oblique: a 1/4 horizontal skew.
    if (match.syntheticItalic) font.setSkewX(-0.25f);
}

sk_sp<SkTypeface> resolveFontFamilyList(std::string_view cssFamily,
                                         SkFontStyle style,
                                         SkFontMgr* mgr) {
    static const std::vector<CustomFontFace> kNone;
    return resolveFontFamilyList(cssFamily, style, mgr, kNone, nullptr);
}

sk_sp<SkTypeface> resolveFontFamilyList(std::string_view cssFamily,
                                         SkFontStyle style,
                                         SkFontMgr* mgr,
                                         const std::vector<CustomFontFace>& faces,
                                         CustomFontMatch* match) {
    if (match) *match = {};
    if (!mgr) return nullptr;

    // CSS font-family is comma-separated — try each name in order.
    std::istringstream stream{std::string(cssFamily)};
    std::string name;
    while (std::getline(stream, name, ',')) {
        while (!name.empty() && (name.front() == ' ' || name.front() == '\'' || name.front() == '"')) name.erase(name.begin());
        while (!name.empty() && (name.back() == ' ' || name.back() == '\'' || name.back() == '"')) name.pop_back();
        if (name.empty()) continue;
        if (!faces.empty()) {
            CustomFontMatch m = matchCustomFontFace(
                faces, name, style.weight(), style.slant() != SkFontStyle::kUpright_Slant);
            if (m.face) {
                if (match) *match = m;
                return m.face->typeface;
            }
        }
        if (const char* resolved = resolveGenericFamily(name)) {
            if (auto tf = sk_sp<SkTypeface>(mgr->matchFamilyStyle(resolved, style))) return tf;
        }
        const auto t0 = std::chrono::steady_clock::now();
        auto tf = sk_sp<SkTypeface>(mgr->matchFamilyStyle(name.c_str(), style));
        noteFontLookup("matchFamilyStyle", name.c_str(), -1,
                       std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        if (tf) return tf;
    }
    return sk_sp<SkTypeface>(mgr->matchFamilyStyle(nullptr, style));
}

} // namespace bro::render
