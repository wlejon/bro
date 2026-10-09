#include "engine/frame_trace.h"

#include "util/json_out.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace bro::engine {

std::vector<FrameRecord> FrameTrace::range(double fromMs, double toMs) const {
    std::vector<FrameRecord> out;
    const size_t n = size();
    out.reserve(std::min<size_t>(n, 2048));
    for (size_t i = head_ - n; i < head_; ++i) {
        const FrameRecord& r = ring_[i % kCapacity];
        if (r.startMs >= fromMs && r.startMs <= toMs) out.push_back(r);
    }
    return out;
}

std::vector<FrameMark> FrameTrace::marks(double fromMs, double toMs) const {
    std::vector<FrameMark> out;
    for (const auto& m : marks_)
        if (m.ms >= fromMs && m.ms <= toMs) out.push_back(m);
    std::sort(out.begin(), out.end(), [](const FrameMark& a, const FrameMark& b) { return a.ms < b.ms; });
    return out;
}

namespace {

// Wall time no phase accounts for.
double otherMs(const FrameRecord& r) {
    const double phases = r.eventsMs + r.controlMs + r.miscMs + r.inputMs + r.tickMs + r.layoutWaitMs + r.recordMs +
                          r.compositeMs + r.presentMs + r.pacingWaitMs;
    return std::max(0.0, (r.endMs - r.startMs) - phases);
}

void writeRecord(util::JsonOut& j, const FrameRecord& r) {
    j.beginObject();
    j.key("frame").integer(static_cast<int64_t>(r.frame));
    j.key("start").number(r.startMs);
    j.key("total").number(r.endMs - r.startMs);
    j.key("events").number(r.eventsMs);
    j.key("control").number(r.controlMs);
    j.key("misc").number(r.miscMs);
    j.key("input").number(r.inputMs);
    j.key("other").number(otherMs(r));
    j.key("tick").number(r.tickMs);
    j.key("js").number(r.jsMs);
    j.key("layoutWait").number(r.layoutWaitMs);
    j.key("record").number(r.recordMs);
    j.key("composite").number(r.compositeMs);
    j.key("present").number(r.presentMs);
    j.key("gpuWait").number(r.gpuWaitMs);
    j.key("flipWait").number(r.flipWaitMs);
    j.key("pacingWait").number(r.pacingWaitMs);
    j.key("style").number(r.styleMs);
    j.key("layout").number(r.layoutPassMs);
    j.key("animTick").number(r.animTickMs);
    j.key("raster").number(r.rasterMs);
    j.key("animations").integer(r.activeAnimations);
    j.key("promoted").integer(r.promotedElements);
    j.key("contentGen").integer(static_cast<int64_t>(r.contentGen));
    j.key("contentTime").number(r.contentTimeMs);
    j.key("vblank").number(r.vblankMs);
    j.key("vblankSeq").integer(r.vblankSeq);
    j.key("keyAt").number(r.keyAtMs);
    j.key("ptyWriteAt").number(r.ptyWriteAtMs);
    j.key("ptyOutputAt").number(r.ptyOutputAtMs);
    j.key("presentAt").number(r.presentAtMs);
    j.key("scanoutMiss").integer(r.scanoutMiss);
    j.key("inputEvents").integer(r.inputEvents);
    j.key("forcedLayouts").integer(r.forcedLayouts);
    j.key("forcedLayoutMs").number(r.forcedLayoutMs);
    j.key("presented").integer(r.presented);
    j.key("layoutRan").boolean(r.layoutRan);
    j.key("layoutPerformed").boolean(r.layoutPerformed);
    j.key("recorded").boolean(r.recorded);
    j.key("rasterSignalled").boolean(r.rasterSignalled);
    j.key("rasterConsumed").boolean(r.rasterConsumed);
    j.key("animating").boolean(r.animating);
    j.endObject();
}

struct Dist {
    double mean = 0, p50 = 0, p95 = 0, max = 0;
};

Dist distOf(std::vector<double> v) {
    Dist d;
    if (v.empty()) return d;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    d.mean = sum / v.size();
    d.p50 = v[v.size() / 2];
    d.p95 = v[std::min(v.size() - 1, static_cast<size_t>(std::ceil(v.size() * 0.95)) - 1)];
    d.max = v.back();
    return d;
}

// Everything the summary reports, computed once for both renderings.
struct Summary {
    size_t frames = 0, presents = 0, animatingPresents = 0, repeats = 0, newContent = 0;
    double durationMs = 0, refreshMs = 0, presentFps = 0, contentFps = 0;
    std::map<int, int> gaps;        // vblank gap (refresh periods) -> count, presents while animating
    std::map<int, int> gapsAll;     // same, every present
    Dist frameTotal, frameInterval, judder;
    std::vector<std::pair<const char*, Dist>> phases;
    std::vector<FrameRecord> worst;
    int missedVblanks = 0;          // while animating: periods with no new content shown
};

Summary summarize(const std::vector<FrameRecord>& recs, double refreshMs, int worstN) {
    Summary s;
    s.frames = recs.size();
    s.refreshMs = refreshMs;
    if (recs.empty()) return s;
    s.durationMs = recs.back().endMs - recs.front().startMs;

    std::vector<double> totals, intervals, judder;
    std::vector<const FrameRecord*> presents;
    for (size_t i = 0; i < recs.size(); ++i) {
        const FrameRecord& r = recs[i];
        totals.push_back(r.endMs - r.startMs);
        if (i > 0) intervals.push_back(r.startMs - recs[i - 1].startMs);
        if (r.presented) presents.push_back(&r);
    }
    s.presents = presents.size();
    auto shownAt = [](const FrameRecord* r) { return r->vblankMs > 0 ? r->vblankMs : r->endMs; };
    auto fresh = [&](size_t k) { return k == 0 || presents[k]->contentGen != presents[k - 1]->contentGen; };
    const FrameRecord* prevNew = nullptr;  // the last present that showed new content
    for (size_t k = 0; k < presents.size(); ++k) {
        const FrameRecord* r = presents[k];
        const FrameRecord* p = k ? presents[k - 1] : nullptr;
        if (p) {
            int gap = 0;
            if (r->vblankSeq && p->vblankSeq) gap = static_cast<int>(r->vblankSeq - p->vblankSeq);
            else if (refreshMs > 0) gap = static_cast<int>(std::lround((shownAt(r) - shownAt(p)) / refreshMs));
            gap = std::clamp(gap, 0, 5);
            ++s.gapsAll[gap];
            if (r->animating || p->animating) ++s.gaps[gap];
        }
        if (r->animating || (p && p->animating)) ++s.animatingPresents;
        if (!fresh(k)) {
            // A stall: the picture held for a present while content was
            // changing just before and just after it (within 3 presents).
            bool before = false, after = false;
            for (size_t b = k; b-- > 0 && k - b <= 3;)
                if (fresh(b) && b > 0) before = true;
            for (size_t a = k + 1; a < presents.size() && a - k <= 3; ++a)
                if (fresh(a)) after = true;
            if (before && after) ++s.repeats;
            continue;
        }
        ++s.newContent;
        // Judder: how far the content clock's step strays from the display's
        // step, between two presents of new animated content close together.
        if (prevNew && r->animating && prevNew->animating && r->contentTimeMs > prevNew->contentTimeMs) {
            const double shown = shownAt(r) - shownAt(prevNew);
            const double stepped = r->contentTimeMs - prevNew->contentTimeMs;
            // Both steps short: a re-raster without a layout pass (a client
            // window's new buffer) shows content laid out long before.
            if (refreshMs <= 0 || (shown <= 6 * refreshMs && stepped <= 6 * refreshMs)) {
                judder.push_back(std::fabs(shown - stepped));
                if (refreshMs > 0)
                    s.missedVblanks += std::max(0, static_cast<int>(std::lround(shown / refreshMs)) - 1);
            }
        }
        prevNew = r;
    }
    if (s.durationMs > 0) {
        s.presentFps = s.presents * 1000.0 / s.durationMs;
        s.contentFps = s.newContent * 1000.0 / s.durationMs;
    }
    s.frameTotal = distOf(totals);
    s.frameInterval = distOf(intervals);
    s.judder = distOf(judder);

    auto phase = [&](const char* name, double FrameRecord::*f) {
        std::vector<double> v;
        v.reserve(recs.size());
        for (const auto& r : recs) v.push_back(r.*f);
        s.phases.emplace_back(name, distOf(std::move(v)));
    };
    phase("events", &FrameRecord::eventsMs);
    phase("control", &FrameRecord::controlMs);
    phase("misc", &FrameRecord::miscMs);
    phase("input", &FrameRecord::inputMs);
    phase("tick", &FrameRecord::tickMs);
    phase("js", &FrameRecord::jsMs);
    phase("layoutWait", &FrameRecord::layoutWaitMs);
    phase("style", &FrameRecord::styleMs);
    phase("layout", &FrameRecord::layoutPassMs);
    phase("record", &FrameRecord::recordMs);
    phase("raster", &FrameRecord::rasterMs);
    phase("composite", &FrameRecord::compositeMs);
    phase("present", &FrameRecord::presentMs);
    phase("gpuWait", &FrameRecord::gpuWaitMs);
    phase("flipWait", &FrameRecord::flipWaitMs);
    phase("pacingWait", &FrameRecord::pacingWaitMs);

    s.worst = recs;
    std::sort(s.worst.begin(), s.worst.end(), [](const FrameRecord& a, const FrameRecord& b) {
        return (a.endMs - a.startMs) > (b.endMs - b.startMs);
    });
    if (s.worst.size() > static_cast<size_t>(worstN)) s.worst.resize(worstN);
    return s;
}

void writeDist(util::JsonOut& j, const Dist& d) {
    j.beginObject();
    j.key("mean").number(d.mean);
    j.key("p50").number(d.p50);
    j.key("p95").number(d.p95);
    j.key("max").number(d.max);
    j.endObject();
}

void writeGaps(util::JsonOut& j, const std::map<int, int>& g) {
    j.beginObject();
    for (auto& [gap, n] : g) j.key(gap >= 5 ? "5+" : std::to_string(gap)).integer(n);
    j.endObject();
}

}  // namespace

std::string FrameTrace::toJson(double fromMs, double toMs) const {
    util::JsonOut j;
    j.beginObject();
    j.key("refreshMs").number(refreshMs_);
    j.key("frames").beginArray();
    for (const auto& r : range(fromMs, toMs)) writeRecord(j, r);
    j.endArray();
    j.key("marks").beginArray();
    for (const auto& m : marks(fromMs, toMs)) {
        j.beginObject();
        j.key("ms").number(m.ms);
        j.key("frame").integer(static_cast<int64_t>(m.frame));
        j.key("label").string(m.label);
        j.endObject();
    }
    j.endArray();
    j.endObject();
    return j.take();
}

std::string FrameTrace::summaryJson(double fromMs, double toMs, int worstN) const {
    const auto recs = range(fromMs, toMs);
    const Summary s = summarize(recs, refreshMs_, worstN);
    util::JsonOut j;
    j.beginObject();
    j.key("frames").integer(static_cast<int64_t>(s.frames));
    j.key("durationMs").number(s.durationMs);
    j.key("refreshMs").number(s.refreshMs);
    j.key("presents").integer(static_cast<int64_t>(s.presents));
    j.key("presentFps").number(s.presentFps, 1);
    j.key("newContentFps").number(s.contentFps, 1);
    j.key("animatingPresents").integer(static_cast<int64_t>(s.animatingPresents));
    j.key("stalls").integer(static_cast<int64_t>(s.repeats));
    j.key("missedVblanksWhileAnimating").integer(s.missedVblanks);
    j.key("vblankGapsWhileAnimating");
    writeGaps(j, s.gaps);
    j.key("vblankGaps");
    writeGaps(j, s.gapsAll);
    j.key("judderMs");
    writeDist(j, s.judder);
    j.key("frameMs");
    writeDist(j, s.frameTotal);
    j.key("frameIntervalMs");
    writeDist(j, s.frameInterval);
    j.key("phases").beginObject();
    for (auto& [name, d] : s.phases) {
        j.key(name);
        writeDist(j, d);
    }
    j.endObject();
    j.key("worst").beginArray();
    for (const auto& r : s.worst) writeRecord(j, r);
    j.endArray();
    j.key("marks").beginArray();
    for (const auto& m : marks(fromMs, toMs)) {
        j.beginObject();
        j.key("ms").number(m.ms);
        j.key("label").string(m.label);
        j.endObject();
    }
    j.endArray();
    j.endObject();
    return j.take();
}

std::string FrameTrace::summaryText(double fromMs, double toMs, int worstN) const {
    const auto recs = range(fromMs, toMs);
    const Summary s = summarize(recs, refreshMs_, worstN);
    std::string out;
    char line[512];
    auto add = [&](const char* fmt, auto... a) {
        std::snprintf(line, sizeof(line), fmt, a...);
        out += line;
    };
    if (recs.empty()) return "no frames in the window\n";
    add("%zu frames over %.0f ms (refresh %.3f ms = %.2f Hz)\n", s.frames, s.durationMs, s.refreshMs,
        s.refreshMs > 0 ? 1000.0 / s.refreshMs : 0.0);
    add("presents %zu (%.1f/s), new content %zu (%.1f/s)\n", s.presents, s.presentFps, s.newContent, s.contentFps);
    add("while animating: %zu presents; stalls (content held mid-motion) %zu; vblanks missed between new "
        "frames %d\n",
        s.animatingPresents, s.repeats, s.missedVblanks);
    auto gaps = [&](const char* what, const std::map<int, int>& g) {
        add("vblank gaps %s:", what);
        for (auto& [gap, n] : g) add(" %s%d=%d", gap >= 5 ? ">=" : "", gap, n);
        out += "\n";
    };
    gaps("(animating)", s.gaps);
    gaps("(all)", s.gapsAll);
    add("judder (|display step - content step|): mean %.2f p95 %.2f max %.2f ms\n", s.judder.mean, s.judder.p95,
        s.judder.max);
    add("frame wall: mean %.2f p95 %.2f max %.2f ms; start-to-start mean %.2f p95 %.2f max %.2f\n",
        s.frameTotal.mean, s.frameTotal.p95, s.frameTotal.max, s.frameInterval.mean, s.frameInterval.p95,
        s.frameInterval.max);
    out += "phase          mean    p95     max\n";
    for (auto& [name, d] : s.phases) add("  %-11s %6.2f  %6.2f  %6.2f\n", name, d.mean, d.p95, d.max);
    add("longest %zu frames:\n", s.worst.size());
    for (const auto& r : s.worst) {
        add("  #%llu t=%.1f total %.2f: events %.2f control %.2f misc %.2f input %.2f tick %.2f (js %.2f) "
            "layoutWait %.2f (style %.2f layout %.2f) record %.2f composite %.2f present %.2f (gpu %.2f flip %.2f) "
            "pacing %.2f other %.2f raster %.2f forced layouts %u (%.2f) anims %u%s%s\n",
            static_cast<unsigned long long>(r.frame), r.startMs - recs.front().startMs, r.endMs - r.startMs,
            r.eventsMs, r.controlMs, r.miscMs, r.inputMs, r.tickMs, r.jsMs, r.layoutWaitMs, r.styleMs,
            r.layoutPassMs, r.recordMs, r.compositeMs, r.presentMs, r.gpuWaitMs, r.flipWaitMs, r.pacingWaitMs,
            otherMs(r), r.rasterMs, r.forcedLayouts, r.forcedLayoutMs, r.activeAnimations,
            r.recorded ? " recorded" : "", r.layoutPerformed ? " layout" : "");
    }
    for (const auto& m : marks(fromMs, toMs)) add("mark t=%.1f %s\n", m.ms - recs.front().startMs, m.label.c_str());
    return out;
}

}  // namespace bro::engine
