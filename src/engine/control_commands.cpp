// The engine's own agent-control commands (docs/agent-control.md): what is
// running, what is on screen, the frame flight recorder, the DOM's boxes, the
// running animations, and waiting for the page to settle. Input is in
// control_input.cpp, recording in control_record.cpp; the commands that need
// the JS realm (eval, dom, inspect) are bronze_host's (host_control.cpp).

#include "engine/control.h"
#include "engine/control_helpers.h"
#include "engine/engine.h"
#include "engine/frame_trace.h"
#include "engine/web_animations.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "platform/control_socket.h"
#include "render/vulkan_presenter.h"
#include "util/json_out.h"
#include "util/time.h"

#if BRO_WITH_DMABUF
#include "render/kms_direct_presenter.h"
#endif

#include "broimage/encode.h"
#include "broimage/geometric.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace bro::engine {

// ---------------------------------------------------------------------------
// Helpers (control_helpers.h)
// ---------------------------------------------------------------------------

std::string describeElement(const dom::Element* el) {
    if (!el) return "<null>";
    std::string tag = el->tagName();
    std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char c) { return std::tolower(c); });
    std::string out = "<" + tag;
    if (!el->id().empty()) out += "#" + el->id();
    const std::string& cls = el->className();
    size_t i = 0;
    while (i < cls.size()) {
        while (i < cls.size() && cls[i] == ' ') ++i;
        size_t j = cls.find(' ', i);
        if (j == std::string::npos) j = cls.size();
        if (j > i) out += "." + cls.substr(i, j - i);
        i = j;
    }
    return out + ">";
}

std::vector<std::pair<dom::Element*, ControlRect>> controlQueryRects(Engine& engine, const std::string& selector,
                                                                     std::string* why) {
    std::vector<std::pair<dom::Element*, ControlRect>> out;
    dom::Document* doc = engine.document();
    if (!doc) {
        if (why) *why = "no document";
        return out;
    }
    std::vector<dom::Element*> els;
    try {
        els = doc->querySelectorAll(selector);
    } catch (...) {
        if (why) *why = "bad selector '" + selector + "'";
        return out;
    }
    if (els.empty()) {
        if (why) *why = "nothing matches '" + selector + "'";
        return out;
    }
    engine.flushLayoutForRead(doc);
    for (dom::Element* el : els) {
        dom::AbsoluteRect r = dom::absoluteBorderBox(el);
        ControlRect c{r.x - doc->viewportScrollX(), r.y - doc->viewportScrollY(), r.width, r.height};
        out.emplace_back(el, c);
    }
    return out;
}

bool controlResolvePoint(Engine& engine, const std::vector<std::string>& args, size_t index, float& x, float& y,
                         size_t* used, std::string* why) {
    auto fail = [&](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    if (index >= args.size()) return fail("missing target (x y, x,y, or a CSS selector)");
    auto num = [](const std::string& s, float& out) {
        if (s.empty()) return false;
        char* end = nullptr;
        out = std::strtof(s.c_str(), &end);
        return end && *end == '\0';
    };
    const std::string& a = args[index];
    const size_t comma = a.find(',');
    if (comma != std::string::npos && num(a.substr(0, comma), x) && num(a.substr(comma + 1), y)) {
        if (used) *used = 1;
        return true;
    }
    if (num(a, x)) {
        if (index + 1 >= args.size() || !num(args[index + 1], y)) return fail("an x coordinate needs a y after it");
        if (used) *used = 2;
        return true;
    }
    std::string err;
    auto rects = controlQueryRects(engine, a, &err);
    if (rects.empty()) return fail(err);
    for (auto& [el, r] : rects) {
        if (r.width <= 0 || r.height <= 0) continue;
        x = r.x + r.width / 2;
        y = r.y + r.height / 2;
        if (used) *used = 1;
        return true;
    }
    return fail("'" + a + "' matches, but nothing it matches has a box (display:none or empty)");
}

bool controlGrabScreen(Engine& engine, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height,
                       std::string* why) {
#if BRO_WITH_DMABUF
    if (engine.displayMode() == DisplayMode::Drm) {
        auto* kms = engine.vulkanPresenter() ? engine.vulkanPresenter()->kmsDirectPresenter() : nullptr;
        if (!kms) {
            if (why) *why = "no KMS presenter";
            return false;
        }
        return kms->readLastFrame(rgba, width, height, why);
    }
#endif
    int w = 0, h = 0;
    if (engine.displayMode() == DisplayMode::Headless) {
        rgba = engine.capturePixels();
        w = engine.framePixelWidth();
        h = engine.framePixelHeight();
    } else {
        rgba = engine.presentedPixels(0, w, h);
    }
    if (rgba.empty() || w <= 0 || h <= 0 || rgba.size() < static_cast<size_t>(w) * h * 4) {
        if (why) *why = "nothing has been presented yet";
        return false;
    }
    width = static_cast<uint32_t>(w);
    height = static_cast<uint32_t>(h);
    return true;
}

std::string controlRuntimePath(const std::string& file) {
    std::string dir = platform::ControlSocket::runtimeDir();
    if (dir.empty()) {
        std::error_code ec;
        dir = std::filesystem::temp_directory_path(ec).string();
    }
    return (std::filesystem::path(std::u8string(dir.begin(), dir.end())) / file).string();
}

namespace {

// ---------------------------------------------------------------------------
// info / now / mark
// ---------------------------------------------------------------------------

const char* modeName(DisplayMode m) {
    switch (m) {
        case DisplayMode::Drm: return "drm";
        case DisplayMode::Headless: return "headless";
        case DisplayMode::Server: return "server";
        default: return "windowed";
    }
}

void cmdInfo(ControlServer& s, const ControlCallPtr& call) {
    Engine& e = s.engine();
    util::JsonOut j;
    j.beginObject();
#if defined(_WIN32)
    j.key("pid").integer(::_getpid());
#else
    j.key("pid").integer(::getpid());
#endif
    j.key("mode").string(modeName(e.displayMode()));
    j.key("app").string(e.appDir());
    j.key("socket").string(s.socketPath());
    j.key("viewport").beginObject().key("width").integer(e.viewportWidth()).key("height").integer(e.viewportHeight());
    j.endObject();
    j.key("framePixels").beginObject().key("width").integer(e.framePixelWidth());
    j.key("height").integer(e.framePixelHeight()).endObject();
    j.key("refreshMs").number(e.frameTrace().refreshPeriodMs());
    j.key("frame").integer(static_cast<int64_t>(e.frameNumber()));
    j.key("nowMs").number(util::currentTimeMs());
    if (const FrameRecord* last = e.frameTrace().last()) {
        j.key("lastFrameMs").number(last->endMs - last->startMs);
    }
    j.key("title").string(e.document() ? e.document()->title() : "");
    j.endObject();
    call->ok(j.take());
}

// ---------------------------------------------------------------------------
// trace: the flight recorder
// ---------------------------------------------------------------------------

void cmdTrace(ControlServer& s, const ControlCallPtr& call) {
    FrameTrace& tr = s.engine().frameTrace();
    const double now = util::currentTimeMs();
    double secs = 5.0;
    if (!call->arg(0).empty()) secs = std::atof(call->arg(0).c_str());
    double from = now - secs * 1000.0;
    double to = now;
    if (!call->option("since").empty()) from = call->number("since", from);
    if (!call->option("until").empty()) to = call->number("until", to);
    const int worst = static_cast<int>(call->number("worst", 8));
    if (call->flag("frames")) return call->ok(tr.toJson(from, to));
    if (call->flag("json")) return call->ok(tr.summaryJson(from, to, worst));
    call->ok(tr.summaryText(from, to, worst));
}

// ---------------------------------------------------------------------------
// rect: boxes of the elements a selector matches
// ---------------------------------------------------------------------------

void cmdRect(ControlServer& s, const ControlCallPtr& call) {
    const std::string sel = call->arg(0);
    if (sel.empty()) return call->fail("usage: rect <selector>");
    std::string why;
    auto rects = controlQueryRects(s.engine(), sel, &why);
    if (rects.empty()) return call->fail(why);
    util::JsonOut j;
    j.beginArray();
    size_t n = 0;
    for (auto& [el, r] : rects) {
        if (++n > 100) break;
        j.beginObject();
        j.key("element").string(describeElement(el));
        j.key("x").number(r.x, 1).key("y").number(r.y, 1);
        j.key("width").number(r.width, 1).key("height").number(r.height, 1);
        j.endObject();
    }
    j.endArray();
    call->ok(j.take());
}

// ---------------------------------------------------------------------------
// screenshot: a PNG of the screen, or of part of it
// ---------------------------------------------------------------------------

void cmdScreenshot(ControlServer& s, const ControlCallPtr& call) {
    Engine& e = s.engine();
    std::string path = call->arg(0);
    if (path.empty()) path = controlRuntimePath("screenshot.png");
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    std::string why;
    if (!controlGrabScreen(e, rgba, w, h, &why)) return call->fail(why);

    // The crop, in frame pixels: a selector's box (padded), or a region in CSS px.
    const float cssToPx = e.viewportWidth() > 0 ? static_cast<float>(w) / e.viewportWidth() : 1.0f;
    int cx = 0, cy = 0, cw = static_cast<int>(w), ch = static_cast<int>(h);
    const std::string sel = call->option("selector");
    const std::string region = call->option("region");
    if (!sel.empty() || !region.empty()) {
        float rx = 0, ry = 0, rw = 0, rh = 0;
        if (!sel.empty()) {
            auto rects = controlQueryRects(e, sel, &why);
            if (rects.empty()) return call->fail(why);
            const ControlRect& r = rects.front().second;
            const float pad = static_cast<float>(call->number("pad", 8));
            rx = r.x - pad;
            ry = r.y - pad;
            rw = r.width + 2 * pad;
            rh = r.height + 2 * pad;
        } else if (std::sscanf(region.c_str(), "%f,%f,%f,%f", &rx, &ry, &rw, &rh) != 4) {
            return call->fail("--region wants x,y,width,height in CSS px");
        }
        int x0 = std::clamp(static_cast<int>(std::floor(rx * cssToPx)), 0, static_cast<int>(w));
        int y0 = std::clamp(static_cast<int>(std::floor(ry * cssToPx)), 0, static_cast<int>(h));
        int x1 = std::clamp(static_cast<int>(std::ceil((rx + rw) * cssToPx)), 0, static_cast<int>(w));
        int y1 = std::clamp(static_cast<int>(std::ceil((ry + rh) * cssToPx)), 0, static_cast<int>(h));
        if (x1 <= x0 || y1 <= y0) return call->fail("the crop is empty (off screen?)");
        cx = x0;
        cy = y0;
        cw = x1 - x0;
        ch = y1 - y0;
    }
    const double scale = std::clamp(call->number("scale", 1.0), 0.05, 4.0);
    const int ow = std::max(1, static_cast<int>(std::lround(cw * scale)));
    const int oh = std::max(1, static_cast<int>(std::lround(ch * scale)));

    // The encode (tens of ms at full size) does not hold the frame loop.
    std::thread([call, path, rgba = std::move(rgba), w, cx, cy, cw, ch, ow, oh]() {
        const uint8_t* src = rgba.data() + (static_cast<size_t>(cy) * w + cx) * 4;
        std::vector<uint8_t> out;
        const uint8_t* pixels = src;
        int stride = static_cast<int>(w) * 4;
        if (ow != cw || oh != ch) {
            out.resize(static_cast<size_t>(ow) * oh * 4);
            broimage::resize_hwc_u8(src, cw, ch, 4, out.data(), ow, oh,
                                    (ow < cw) ? broimage::Filter::Area : broimage::Filter::Bilinear, stride, 0);
            pixels = out.data();
            stride = ow * 4;
        }
        std::error_code ec;
        const auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        if (!broimage::encode_png_file(path, pixels, ow, oh, 4, stride)) return call->fail("writing " + path + " failed");
        util::JsonOut j;
        j.beginObject().key("path").string(path).key("width").integer(ow).key("height").integer(oh);
        j.key("crop").beginObject().key("x").integer(cx).key("y").integer(cy).key("width").integer(cw);
        j.key("height").integer(ch).endObject().endObject();
        call->ok(j.take());
    }).detach();
}

// ---------------------------------------------------------------------------
// animations: what is running now
// ---------------------------------------------------------------------------

void cmdAnimations(ControlServer& s, const ControlCallPtr& call) {
    Engine& e = s.engine();
    WebAnimationManager& wam = e.webAnimationManager();
    const double now = e.timeNowMs();
    const bool all = call->flag("all");
    util::JsonOut j;
    j.beginArray();
    for (uint64_t id : wam.allAnimations(now)) {
        const WebAnimation* a = wam.find(id);
        if (!a) continue;
        const char* state = wam.playState(*a, now);
        if (!all && std::string(state) != "running") continue;
        j.beginObject();
        j.key("id").integer(static_cast<int64_t>(id));
        j.key("kind").string(a->isCssTransition ? "transition" : a->isCssAnimation ? "css-animation" : "script");
        if (a->isCssTransition) j.key("property").string(a->cssProperty);
        if (a->isCssAnimation) j.key("name").string(a->cssName);
        j.key("target").string(describeElement(wam.resolveElement(*a)));
        j.key("state").string(state);
        auto ct = a->currentTimeMs(now);
        if (ct) j.key("currentTime").number(*ct, 1);
        else j.key("currentTime").null();
        j.key("duration").number(a->duration, 1);
        j.key("delay").number(a->delay, 1);
        j.key("iterations").number(a->iterations, 2);
        j.key("rate").number(a->playbackRate, 2);
        j.key("properties").beginArray();
        std::vector<std::string> props;
        for (const auto& kf : a->keyframes)
            for (const auto& [p, v] : kf.props)
                if (std::find(props.begin(), props.end(), p) == props.end()) props.push_back(p);
        for (const auto& p : props) j.string(p);
        j.endArray();
        j.endObject();
    }
    j.endArray();
    call->ok(j.take());
}

// ---------------------------------------------------------------------------
// wait-idle / wait: let the page settle, or something appear
// ---------------------------------------------------------------------------

void cmdWaitIdle(ControlServer& s, const ControlCallPtr& call) {
    Engine& e = s.engine();
    ControlServer* server = &s;
    const double start = s.clockMs();
    const double timeout = call->number("timeout", 5000);
    const int quietFrames = static_cast<int>(call->number("frames", 3));
    auto quiet = std::make_shared<int>(0);
    auto lastSeen = std::make_shared<uint64_t>(e.frameNumber());
    s.addTicker([&e, server, call, start, timeout, quietFrames, quiet, lastSeen]() {
        if (call->done()) return true;
        // Judge the frame that just finished (the ticker runs at the start of the next).
        const FrameRecord* last = e.frameTrace().last();
        if (last && last->frame != *lastSeen) {
            *lastSeen = last->frame;
            const bool busy = last->animating || last->layoutRan || last->rasterSignalled || last->inputEvents;
            *quiet = busy ? 0 : *quiet + 1;
        }
        const double elapsed = server->clockMs() - start;
        if (*quiet >= quietFrames) {
            call->ok("{\"idle\":true,\"waitedMs\":" + std::to_string(static_cast<int>(elapsed)) + "}");
            return true;
        }
        if (elapsed > timeout) {
            call->fail("still busy after " + std::to_string(static_cast<int>(timeout)) +
                       " ms (an animation or a page that re-renders every frame?)");
            return true;
        }
        return false;
    });
}

void cmdWait(ControlServer& s, const ControlCallPtr& call) {
    Engine& e = s.engine();
    const std::string sel = call->arg(0);
    if (sel.empty()) return call->fail("usage: wait <selector> [--gone] [--timeout=ms]");
    const bool gone = call->flag("gone");
    ControlServer* server = &s;
    const double start = s.clockMs();
    const double timeout = call->number("timeout", 5000);
    s.addTicker([&e, server, call, sel, gone, start, timeout]() {
        if (call->done()) return true;
        std::string why;
        auto rects = controlQueryRects(e, sel, &why);
        bool visible = false;
        for (auto& [el, r] : rects)
            if (r.width > 0 && r.height > 0) visible = true;
        const double elapsed = server->clockMs() - start;
        if (visible != gone) {
            call->ok("{\"waitedMs\":" + std::to_string(static_cast<int>(elapsed)) + "}");
            return true;
        }
        if (elapsed > timeout) {
            call->fail(std::string(gone ? "still visible: " : "not visible: ") + sel);
            return true;
        }
        return false;
    });
}

}  // namespace

void registerEngineControlCommands(ControlServer& s) {
    s.registerCommand("info", " what is running: mode, app, viewport, refresh, frame number, socket",
                      [&s](const ControlCallPtr& c) { cmdInfo(s, c); });
    s.registerCommand("now", " the clock the trace uses (CLOCK_MONOTONIC ms), for --since",
                      [](const ControlCallPtr& c) { c->ok(std::to_string(util::currentTimeMs())); });
    s.registerCommand("mark", "<label>  put a labelled mark in the frame trace",
                      [&s](const ControlCallPtr& c) {
                          s.engine().frameTrace().mark(util::currentTimeMs(), c->arg(0, "mark"));
                          c->ok();
                      });
    s.registerCommand("trace",
                      "[seconds=5] [--since=ms] [--until=ms] [--json] [--frames] [--worst=N]  frame pacing over "
                      "the last seconds: presents, vblank gaps, repeated content, judder, phase times, longest "
                      "frames (--json: the same as JSON; --frames: every frame record)",
                      [&s](const ControlCallPtr& c) { cmdTrace(s, c); });
    s.registerCommand("rect", "<selector>  client rects (CSS px) of every match",
                      [&s](const ControlCallPtr& c) { cmdRect(s, c); });
    s.registerCommand("screenshot",
                      "[path] [--selector=S [--pad=8]] [--region=x,y,w,h] [--scale=F]  PNG of the screen (the "
                      "scanout buffer under DRM), cropped to an element or region (CSS px) and scaled",
                      [&s](const ControlCallPtr& c) { cmdScreenshot(s, c); });
    s.registerCommand("animations", "[--all]  running animations and transitions: target, properties, time",
                      [&s](const ControlCallPtr& c) { cmdAnimations(s, c); });
    s.registerCommand("wait-idle",
                      "[--timeout=5000] [--frames=3]  until N frames in a row lay out, animate and render nothing",
                      [&s](const ControlCallPtr& c) { cmdWaitIdle(s, c); });
    s.registerCommand("wait", "<selector> [--gone] [--timeout=5000]  until a match has a box (or none does)",
                      [&s](const ControlCallPtr& c) { cmdWait(s, c); });
}

}  // namespace bro::engine
