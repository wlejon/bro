#include "render/system_font_mgr.h"

#include "util/log.h"

#include <include/core/SkFont.h>
#include <include/core/SkFontMetrics.h>
#include <include/core/SkFontStyle.h>
#include <include/core/SkFontTypes.h>
#include <include/core/SkTypeface.h>
#ifdef _WIN32
#include <include/ports/SkTypeface_win.h>
#elif defined(__APPLE__)
#include <include/ports/SkFontMgr_mac_ct.h>
#else
#include <include/ports/SkFontMgr_fontconfig.h>
#include <include/ports/SkFontScanner_FreeType.h>
#endif

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace bro::render {

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

enum class State { Idle, Building, Ready };

struct Shared {
    std::mutex mu;
    std::condition_variable cv;
    State state = State::Idle;
    sk_sp<SkFontMgr> mgr;
    std::thread worker;

    ~Shared() {
        if (worker.joinable()) worker.join();
    }
};

Shared& shared() {
    static Shared s;
    return s;
}

sk_sp<SkFontMgr> makePlatformFontMgr() {
#ifdef _WIN32
    return SkFontMgr_New_DirectWrite();
#elif defined(__APPLE__)
    return SkFontMgr_New_CoreText(nullptr);
#else
    return SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
#endif
}

// Everything the first layout would otherwise do cold: the system collection
// and fallback map (the manager itself), resolving the default face by name,
// one fallback query, and a scaler context with real glyph metrics.
sk_sp<SkFontMgr> buildWarm(const char* where) {
    const auto t0 = Clock::now();
    sk_sp<SkFontMgr> mgr = makePlatformFontMgr();
    const double tMgr = msSince(t0);

    auto t = Clock::now();
    SkFontStyle style = SkFontStyle::Normal();
    sk_sp<SkTypeface> face = mgr ? mgr->matchFamilyStyle(nullptr, style) : nullptr;
#ifdef _WIN32
    if (mgr) {
        if (auto named = mgr->matchFamilyStyle("Segoe UI", style)) face = named;
        mgr->matchFamilyStyle("Arial", style);
    }
#endif
    const double tMatch = msSince(t);

    t = Clock::now();
    if (mgr) mgr->matchFamilyStyleCharacter(nullptr, style, nullptr, 0, 0x2022);
    const double tFallback = msSince(t);

    t = Clock::now();
    if (face) {
        SkFont font(face, 16.0f);
        SkFontMetrics fm;
        font.getMetrics(&fm);
        SkGlyphID glyphs[8];
        SkScalar widths[8];
        size_t n = font.textToGlyphs("Wg0 .,:A", 8, SkTextEncoding::kUTF8, {glyphs, 8});
        if (n > 8) n = 8;
        font.getWidths({glyphs, n}, {widths, n});
    }
    const double tGlyphs = msSince(t);

    LOG_INFO("System fonts ready %s in %.1f ms (manager %.1f, match %.1f, fallback %.1f, glyphs %.1f)",
             where, msSince(t0), tMgr, tMatch, tFallback, tGlyphs);
    return mgr;
}

void publish(Shared& s, sk_sp<SkFontMgr> mgr) {
    {
        std::lock_guard lock(s.mu);
        s.mgr = std::move(mgr);
        s.state = State::Ready;
    }
    s.cv.notify_all();
}

} // namespace

void noteFontLookup(const char* what, const char* name, int32_t codepoint, double ms) {
    if (ms < 20.0) return;
    if (codepoint >= 0) {
        LOG_WARN("Slow font lookup: %s('%s', U+%04X) took %.1f ms", what, name ? name : "", codepoint, ms);
    } else {
        LOG_WARN("Slow font lookup: %s('%s') took %.1f ms", what, name ? name : "", ms);
    }
}

void prewarmSystemFontMgr() {
    Shared& s = shared();
    std::lock_guard lock(s.mu);
    if (s.state != State::Idle) return;
    s.state = State::Building;
    s.worker = std::thread([&s] { publish(s, buildWarm("off-thread")); });
}

SkFontMgr* systemFontMgr() {
    Shared& s = shared();
    std::unique_lock lock(s.mu);
    if (s.state == State::Ready) return s.mgr.get();
    if (s.state == State::Idle) {
        s.state = State::Building;
        lock.unlock();
        publish(s, buildWarm("on first use"));
        lock.lock();
        return s.mgr.get();
    }
    const auto t0 = Clock::now();
    s.cv.wait(lock, [&s] { return s.state == State::Ready; });
    const double waited = msSince(t0);
    if (waited >= 1.0) LOG_INFO("First text use waited %.1f ms for system fonts", waited);
    return s.mgr.get();
}

} // namespace bro::render
