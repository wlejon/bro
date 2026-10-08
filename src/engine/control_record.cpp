// Agent-control screen recording (docs/agent-control.md): `record <seconds>
// <dir>` keeps every composited frame for a while — scaled down, copied out
// of the scanout buffer by the GPU as part of the frame (render/
// scanout_capture.h) — together with the vblank each landed on, then writes
// a directory a tool can read without watching a video:
//
//   frames/NNNNN.png   every presented frame
//   frames.ffconcat    the frames with their on-screen durations, for ffmpeg
//                      (bro-ctl record makes video.mp4 from it)
//   contact.png        a contact sheet of the frames around the motion,
//                      labelled with their vblank gaps; a repeated frame is
//                      outlined red, one that followed a missed vblank amber
//   timeline.json      per frame: vblank time and counter, gap, how much of
//                      the picture changed and where, plus the flight
//                      recorder's records and summary for the same window
//   summary.txt        the pacing in a few lines
//
// Under DRM only (the frames are the scanout's).

#include "engine/control.h"
#include "engine/control_helpers.h"
#include "engine/engine.h"
#include "engine/frame_trace.h"
#include "render/system_font_mgr.h"
#include "render/vulkan_presenter.h"
#include "util/json_out.h"
#include "util/log.h"
#include "util/time.h"

#if BRO_WITH_DMABUF
#include "render/kms_direct_presenter.h"
#include "render/scanout_capture.h"
#endif

#include "broimage/encode.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkFont.h>
#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkSurface.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

namespace bro::engine {

namespace {

struct CapturedFrame {
    std::vector<uint8_t> rgba;
    double vblankMs = 0;
    uint32_t seq = 0;
    uint64_t frame = 0;
    // Filled at write time.
    double diff = 0;        // mean |delta| per channel, 0..255
    double changed = 0;     // fraction of pixels that changed visibly
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;  // bounding box of the change
    int gap = 1;            // vblanks since the previous frame
};

struct Recording {
    std::string dir;
    double startMs = 0, durationMs = 0;
    uint32_t width = 0, height = 0;
    double scale = 0.5;
    size_t maxFrames = 0;
    std::vector<CapturedFrame> frames;
    ControlCallPtr call;
    bool overflow = false;
#if BRO_WITH_DMABUF
    std::unique_ptr<render::ScanoutCapture> capture;
#endif
};

std::shared_ptr<Recording> g_active;

// Visible change between two frames, on a 4-px grid.
void measureChange(const CapturedFrame& prev, CapturedFrame& cur, uint32_t w, uint32_t h) {
    double sum = 0;
    size_t samples = 0, changed = 0;
    int bx0 = static_cast<int>(w), by0 = static_cast<int>(h), bx1 = -1, by1 = -1;
    for (uint32_t y = 0; y < h; y += 4) {
        const uint8_t* a = prev.rgba.data() + static_cast<size_t>(y) * w * 4;
        const uint8_t* b = cur.rgba.data() + static_cast<size_t>(y) * w * 4;
        for (uint32_t x = 0; x < w; x += 4) {
            int d = 0;
            for (int c = 0; c < 3; ++c) d = std::max(d, std::abs(int(a[x * 4 + c]) - int(b[x * 4 + c])));
            sum += d;
            ++samples;
            if (d > 6) {
                ++changed;
                bx0 = std::min(bx0, static_cast<int>(x));
                by0 = std::min(by0, static_cast<int>(y));
                bx1 = std::max(bx1, static_cast<int>(x));
                by1 = std::max(by1, static_cast<int>(y));
            }
        }
    }
    cur.diff = samples ? sum / samples : 0;
    cur.changed = samples ? static_cast<double>(changed) / samples : 0;
    if (bx1 >= 0) {
        cur.bx0 = bx0;
        cur.by0 = by0;
        cur.bx1 = std::min<int>(bx1 + 4, w);
        cur.by1 = std::min<int>(by1 + 4, h);
    }
}

// A stall: no change while the frames around it (within 3) changed.
bool isStall(const std::vector<CapturedFrame>& F, size_t i) {
    if (i == 0 || F[i].changed > 0) return false;
    bool before = false, after = false;
    for (size_t b = i; b-- > 1 && i - b <= 3;)
        if (F[b].changed > 0) before = true;
    for (size_t a = i + 1; a < F.size() && a - i <= 3; ++a)
        if (F[a].changed > 0) after = true;
    return before && after;
}

// The contact sheet: up to `count` frames from `first`, 6 to a row, each
// cropped to `crop` (the region that moved).
bool writeContactSheet(const Recording& r, size_t first, size_t count, SkIRect crop, const std::string& path) {
    const int cols = 6;
    const int tw = 300;
    const int th = static_cast<int>(std::lround(tw * double(crop.height()) / crop.width()));
    const int label = 18;
    const int rows = static_cast<int>((count + cols - 1) / cols);
    const int W = cols * (tw + 6) + 6, H = rows * (th + label + 6) + 6;
    sk_sp<SkSurface> surf = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(W, H));
    if (!surf) return false;
    SkCanvas* c = surf->getCanvas();
    c->clear(SkColorSetRGB(24, 24, 28));
    sk_sp<SkTypeface> tf;
    if (SkFontMgr* mgr = render::systemFontMgr()) tf = mgr->matchFamilyStyle("monospace", SkFontStyle());
    SkFont font(tf, 13);
    SkPaint text;
    text.setColor(SK_ColorWHITE);
    text.setAntiAlias(true);
    for (size_t k = 0; k < count; ++k) {
        const CapturedFrame& f = r.frames[first + k];
        const int col = static_cast<int>(k % cols), row = static_cast<int>(k / cols);
        const float x = 6 + col * (tw + 6.0f), y = 6 + row * (th + label + 6.0f);
        SkImageInfo info = SkImageInfo::Make(r.width, r.height, kRGBA_8888_SkColorType, kOpaque_SkAlphaType);
        SkPixmap pm(info, f.rgba.data(), r.width * 4);
        sk_sp<SkImage> img = SkImages::RasterFromPixmapCopy(pm);
        if (img)
            c->drawImageRect(img, SkRect::Make(crop), SkRect::MakeXYWH(x, y + label, tw, th),
                             SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone), nullptr,
                             SkCanvas::kStrict_SrcRectConstraint);
        const bool repeat = isStall(r.frames, first + k);
        const bool late = f.gap > 1;
        if (repeat || late) {
            SkPaint border;
            border.setStyle(SkPaint::kStroke_Style);
            border.setStrokeWidth(3);
            border.setColor(repeat ? SkColorSetRGB(240, 60, 60) : SkColorSetRGB(250, 180, 40));
            c->drawRect(SkRect::MakeXYWH(x + 1.5f, y + label + 1.5f, tw - 3, th - 3), border);
        }
        char buf[96];
        const double t = f.vblankMs - r.frames[first].vblankMs;
        std::snprintf(buf, sizeof(buf), "#%zu +%.1fms%s%s", first + k, t,
                      late ? (std::string(" gap ") + std::to_string(f.gap)).c_str() : "", repeat ? " STALL" : "");
        c->drawString(buf, x, y + 13, font, text);
    }
    SkPixmap out;
    if (!surf->peekPixels(&out)) return false;
    // N32 is BGRA on little-endian Linux; encode wants RGBA.
    std::vector<uint8_t> rgba(static_cast<size_t>(W) * H * 4);
    for (int yy = 0; yy < H; ++yy) {
        const uint8_t* src = static_cast<const uint8_t*>(out.addr(0, yy));
        uint8_t* dst = rgba.data() + static_cast<size_t>(yy) * W * 4;
        for (int xx = 0; xx < W; ++xx) {
            if (out.colorType() == kBGRA_8888_SkColorType) {
                dst[xx * 4 + 0] = src[xx * 4 + 2];
                dst[xx * 4 + 1] = src[xx * 4 + 1];
                dst[xx * 4 + 2] = src[xx * 4 + 0];
            } else {
                std::copy(src + xx * 4, src + xx * 4 + 3, dst + xx * 4);
            }
            dst[xx * 4 + 3] = 255;
        }
    }
    return broimage::encode_png_file(path, rgba.data(), W, H, 4);
}

// Off the engine thread: everything that is files.
void writeRecording(std::shared_ptr<Recording> r, double refreshMs, std::string traceSummary,
                    std::string traceText, std::string traceFrames) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(r->dir) / "frames", ec);
    if (ec) return r->call->fail("cannot create " + r->dir + ": " + ec.message());

    const uint32_t w = r->width, h = r->height;
    auto& F = r->frames;
    for (size_t i = 0; i < F.size(); ++i) {
        if (i > 0) {
            measureChange(F[i - 1], F[i], w, h);
            if (F[i].seq && F[i - 1].seq) F[i].gap = static_cast<int>(F[i].seq - F[i - 1].seq);
            else if (refreshMs > 0) F[i].gap = static_cast<int>(std::lround((F[i].vblankMs - F[i - 1].vblankMs) / refreshMs));
        }
    }

    // PNGs, in parallel.
    std::atomic<size_t> next{0};
    std::vector<std::thread> workers;
    const unsigned n = std::max(2u, std::min(8u, std::thread::hardware_concurrency()));
    for (unsigned t = 0; t < n; ++t) {
        workers.emplace_back([&] {
            for (size_t i; (i = next.fetch_add(1)) < F.size();) {
                // BGRX as captured -> RGBA.
                uint8_t* p = F[i].rgba.data();
                for (size_t k = 0, n = static_cast<size_t>(w) * h; k < n; ++k) {
                    std::swap(p[k * 4 + 0], p[k * 4 + 2]);
                    p[k * 4 + 3] = 255;
                }
                char name[32];
                std::snprintf(name, sizeof(name), "%05zu.png", i);
                broimage::encode_png_file((fs::path(r->dir) / "frames" / name).string(), F[i].rgba.data(),
                                          static_cast<int>(w), static_cast<int>(h), 4);
            }
        });
    }
    for (auto& t : workers) t.join();

    // ffconcat: each frame for as long as it was on screen.
    {
        std::ofstream ff(fs::path(r->dir) / "frames.ffconcat");
        ff << "ffconcat version 1.0\n";
        for (size_t i = 0; i < F.size(); ++i) {
            const double dur = (i + 1 < F.size()) ? std::max(1, F[i + 1].gap) * refreshMs : refreshMs;
            char line[96];
            std::snprintf(line, sizeof(line), "file frames/%05zu.png\nduration %.6f\n", i, dur / 1000.0);
            ff << line;
        }
        if (!F.empty()) {
            char line[64];
            std::snprintf(line, sizeof(line), "file frames/%05zu.png\n", F.size() - 1);
            ff << line;
        }
    }

    // The contact sheet: from just before the first change, up to 48 frames,
    // cropped to the region that changed (with a margin), so the motion is
    // big enough to see.
    size_t first = 0;
    for (size_t i = 1; i < F.size(); ++i)
        if (F[i].changed > 0) {
            first = i > 2 ? i - 2 : 0;
            break;
        }
    const size_t count = std::min<size_t>(48, F.size() - first);
    SkIRect crop = SkIRect::MakeEmpty();
    for (size_t i = first; i < first + count; ++i)
        if (F[i].bx1 > F[i].bx0) crop.join(SkIRect::MakeLTRB(F[i].bx0, F[i].by0, F[i].bx1, F[i].by1));
    if (crop.isEmpty() || crop.width() * crop.height() > static_cast<int>(w * h) * 3 / 4) {
        crop = SkIRect::MakeWH(static_cast<int>(w), static_cast<int>(h));
    } else {
        crop.outset(24, 24);
        if (!crop.intersect(SkIRect::MakeWH(static_cast<int>(w), static_cast<int>(h))))
            crop = SkIRect::MakeWH(static_cast<int>(w), static_cast<int>(h));
        // Not narrower than 16:9 tall strips need: at least a third of the width.
        if (crop.width() < static_cast<int>(w) / 3) crop.outset((static_cast<int>(w) / 3 - crop.width()) / 2, 0);
        if (!crop.intersect(SkIRect::MakeWH(static_cast<int>(w), static_cast<int>(h))))
            crop = SkIRect::MakeWH(static_cast<int>(w), static_cast<int>(h));
    }
    if (count) writeContactSheet(*r, first, count, crop, (fs::path(r->dir) / "contact.png").string());

    // timeline.json + summary.txt
    size_t repeats = 0, late = 0, missed = 0, moving = 0;
    {
        util::JsonOut j;
        j.beginObject();
        j.key("width").integer(w).key("height").integer(h).key("scale").number(r->scale);
        j.key("refreshMs").number(refreshMs);
        j.key("startMs").number(F.empty() ? 0 : F.front().vblankMs);
        j.key("truncated").boolean(r->overflow);
        j.key("contactSheetFirst").integer(static_cast<int64_t>(first));
        j.key("contactSheetCrop").beginArray().integer(crop.x()).integer(crop.y()).integer(crop.width());
        j.integer(crop.height()).endArray();
        j.key("frames").beginArray();
        for (size_t i = 0; i < F.size(); ++i) {
            const auto& f = F[i];
            j.beginObject();
            j.key("i").integer(static_cast<int64_t>(i));
            j.key("t").number(f.vblankMs - F.front().vblankMs, 2);
            j.key("vblankMs").number(f.vblankMs, 3);
            j.key("seq").integer(f.seq);
            j.key("gap").integer(f.gap);
            j.key("frame").integer(static_cast<int64_t>(f.frame));
            j.key("diff").number(f.diff, 2);
            j.key("changed").number(f.changed, 4);
            if (f.bx1 > f.bx0) {
                j.key("changeBox").beginArray().integer(f.bx0).integer(f.by0).integer(f.bx1 - f.bx0);
                j.integer(f.by1 - f.by0).endArray();
            }
            j.endObject();
            if (i > 0) {
                if (f.gap > 1) {
                    ++late;
                    missed += f.gap - 1;
                }
                if (f.changed > 0) ++moving;
                // A repeat that matters: the picture stood still for a
                // frame while it was moving before and after.
                if (isStall(F, i)) ++repeats;
            }
        }
        j.endArray();
        j.key("trace").raw(traceSummary.empty() ? "null" : traceSummary);
        j.key("records").raw(traceFrames.empty() ? "null" : traceFrames);
        j.endObject();
        std::ofstream(fs::path(r->dir) / "timeline.json") << j.str();
    }
    char head[512];
    std::snprintf(head, sizeof(head),
                  "%zu frames presented over %.0f ms (%ux%u, refresh %.3f ms)\n"
                  "frames that changed: %zu; stalls (a frame that repeated its predecessor mid-motion): %zu\n"
                  "presents after a missed vblank: %zu (%zu vblanks missed)%s\n\n",
                  F.size(), F.empty() ? 0.0 : F.back().vblankMs - F.front().vblankMs, w, h, refreshMs, moving,
                  repeats, late, missed, r->overflow ? "\n(stopped early: memory cap)" : "");
    std::string summary = std::string(head) + "flight recorder for the same window:\n" + traceText;
    std::ofstream(fs::path(r->dir) / "summary.txt") << summary;

    util::JsonOut j;
    j.beginObject();
    j.key("dir").string(r->dir);
    j.key("frames").integer(static_cast<int64_t>(F.size()));
    j.key("changed").integer(static_cast<int64_t>(moving));
    j.key("stalls").integer(static_cast<int64_t>(repeats));
    j.key("latePresents").integer(static_cast<int64_t>(late));
    j.key("missedVblanks").integer(static_cast<int64_t>(missed));
    j.key("width").integer(w).key("height").integer(h);
    j.key("contactSheet").string((fs::path(r->dir) / "contact.png").string());
    j.endObject();
    r->call->ok(j.take());
}

#if BRO_WITH_DMABUF
void finish(Engine& e, std::shared_ptr<Recording> r) {
    auto* kms = e.vulkanPresenter() ? e.vulkanPresenter()->kmsDirectPresenter() : nullptr;
    if (kms) kms->setFrameTap(nullptr);
    r->capture.reset();
    g_active.reset();
    const double from = r->startMs, to = util::currentTimeMs();
    FrameTrace& tr = e.frameTrace();
    std::thread(writeRecording, r, tr.refreshPeriodMs(), tr.summaryJson(from, to), tr.summaryText(from, to),
                tr.toJson(from, to))
        .detach();
}
#endif

void cmdRecord(ControlServer& s, const ControlCallPtr& call) {
#if BRO_WITH_DMABUF
    Engine& e = s.engine();
    auto* kms = e.vulkanPresenter() ? e.vulkanPresenter()->kmsDirectPresenter() : nullptr;
    if (e.displayMode() != DisplayMode::Drm || !kms || !e.vulkanContext())
        return call->fail("record needs bro --drm (it records the scanout)");
    if (g_active) return call->fail("a recording is already running");
    const double secs = std::clamp(std::atof(call->arg(0, "3").c_str()), 0.1, 60.0);
    std::string dir = call->arg(1);
    if (dir.empty()) dir = controlRuntimePath("record");
    const double scale = std::clamp(call->number("scale", 0.5), 0.1, 1.0);

    auto r = std::make_shared<Recording>();
    r->dir = dir;
    r->durationMs = secs * 1000.0;
    r->scale = scale;
    r->call = call;
    const uint32_t fw = static_cast<uint32_t>(e.framePixelWidth()), fh = static_cast<uint32_t>(e.framePixelHeight());
    r->width = std::max(16u, static_cast<uint32_t>(std::lround(fw * scale)) & ~1u);
    r->height = std::max(16u, static_cast<uint32_t>(std::lround(fh * scale)) & ~1u);
    const size_t frameBytes = static_cast<size_t>(r->width) * r->height * 4;
    const size_t capBytes = static_cast<size_t>(call->number("max-mb", 3072)) << 20;
    r->maxFrames = std::max<size_t>(1, capBytes / frameBytes);
    r->frames.reserve(std::min<size_t>(r->maxFrames, static_cast<size_t>(secs * 250)));

    std::weak_ptr<Recording> weak = r;
    Engine* ep = &e;
    r->capture = std::make_unique<render::ScanoutCapture>(
        *e.vulkanContext(), r->width, r->height,
        [weak, ep](const uint8_t* bgra, uint32_t w, uint32_t h, const render::KmsDirectPresenter::FlipInfo& flip) {
            auto rec = weak.lock();
            if (!rec) return;
            if (rec->frames.size() >= rec->maxFrames) {
                rec->overflow = true;
                return;
            }
            // A plain copy here, on the frame's thread; the swizzle to RGBA
            // happens when the files are written.
            CapturedFrame f;
            f.rgba.assign(bgra, bgra + static_cast<size_t>(w) * h * 4);
            f.vblankMs = flip.vblankMs;
            f.seq = flip.sequence;
            f.frame = ep->frameNumber();
            rec->frames.push_back(std::move(f));
        });
    if (!r->capture->valid()) return call->fail("could not allocate the capture image/buffer");
    r->startMs = util::currentTimeMs();
    kms->setFrameTap(r->capture.get());
    g_active = r;
    s.addTicker([&e, r]() {
        if (util::currentTimeMs() - r->startMs < r->durationMs && !r->overflow) return false;
        finish(e, r);
        return true;
    });
#else
    (void)s;
    call->fail("record needs a build with DMA-BUF / DRM support");
#endif
}

}  // namespace

void registerControlRecordCommands(ControlServer& s) {
    s.registerCommand("record",
                      "[seconds=3] [dir] [--scale=0.5] [--max-mb=3072]  every presented frame for a while, with "
                      "its vblank: frames/, contact.png, timeline.json, summary.txt, frames.ffconcat (DRM only)",
                      [&s](const ControlCallPtr& c) { cmdRecord(s, c); });
}

}  // namespace bro::engine
