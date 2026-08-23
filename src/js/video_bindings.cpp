#if BRO_WITH_VIDEO

#include "js/video_bindings.h"
#include "canvas/canvas_scene.h"
#include "dom/element.h"
#include "engine/engine.h"
#include "js/dom_bindings_internal.h"
#include "video/gif_encoder.h"
#include "video/webm_encoder.h"
#include <qjsbind/qjsbind.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace bro::js {


namespace {

// Engine pointer stash key, mirroring the one used by headless_bindings /
// installCanvasSnapshotBinding. We only read it; whoever installs the engine
// (engine_init in both modes, headless main in headless mode) is the writer.
constexpr const char* kEngineKey = "__bro_engine_ptr";

engine::Engine* getEngineForCtx(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, kEngineKey);
    engine::Engine* e = nullptr;
    if (JS_IsNumber(val)) {
        int64_t ptr = 0;
        JS_ToInt64(ctx, &ptr, val);
        e = reinterpret_cast<engine::Engine*>(static_cast<intptr_t>(ptr));
    }
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, global);
    return e;
}

video::WebmEncoder::Quality parseQuality(const std::string& s) {
    if (s == "realtime") return video::WebmEncoder::Quality::Realtime;
    if (s == "best")     return video::WebmEncoder::Quality::Best;
    return video::WebmEncoder::Quality::Good;  // default + "good"
}

// JS-owned wrapper around the C++ encoder. Holds a unique_ptr so finishing
// or destroying the JS object also cleans up libvpx + the file handle.
struct EncoderData {
    std::unique_ptr<video::WebmEncoder> enc;
    int width = 0;
    int height = 0;
    int audioChannels = 0;  // 0 = no audio track
    std::string lastErr;
};

using ED = EncoderData;

// JS: new VideoEncoder({ path, width, height, fps?, fpsDen?, bitrateKbps?,
//                        quality?, keyframeIntervalSec?, threads? })
ED* js_videoEncoderCtor(JSContext* ctx, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        JS_ThrowTypeError(ctx, "VideoEncoder requires a config object");
        return nullptr;
    }
    auto getInt = [&](const char* key, int dflt) -> int {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], key);
        int out = dflt;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            int32_t i = dflt;
            JS_ToInt32(ctx, &i, v);
            out = i;
        }
        JS_FreeValue(ctx, v);
        return out;
    };
    auto getStr = [&](const char* key) -> std::string {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], key);
        std::string out;
        if (JS_IsString(v)) {
            const char* s = JS_ToCString(ctx, v);
            if (s) { out = s; JS_FreeCString(ctx, s); }
        }
        JS_FreeValue(ctx, v);
        return out;
    };

    std::string path = getStr("path");
    if (path.empty()) {
        JS_ThrowTypeError(ctx, "VideoEncoder: path is required");
        return nullptr;
    }
    video::WebmEncoder::Config cfg;
    cfg.width = getInt("width", 0);
    cfg.height = getInt("height", 0);
    cfg.fpsNum = getInt("fps", 30);
    cfg.fpsDen = getInt("fpsDen", 1);
    cfg.targetBitrateKbps = getInt("bitrateKbps", 0);
    cfg.keyframeIntervalSec = getInt("keyframeIntervalSec", 2);
    cfg.threads = getInt("threads", 0);
    // Metadata, not a pass over the pixels: the frames go in as they are
    // handed over and a player turns them. 0 writes nothing.
    cfg.rotationDegrees = getInt("rotation", 0);
    cfg.quality = parseQuality(getStr("quality"));
    cfg.audioSampleRate  = getInt("audioSampleRate", 0);
    cfg.audioChannels    = getInt("audioChannels", 2);
    cfg.audioBitrateKbps = getInt("audioBitrateKbps", 96);

    std::string err;
    auto enc = video::WebmEncoder::create(path, cfg, &err);
    if (!enc) {
        JS_ThrowInternalError(ctx, "VideoEncoder open failed: %s", err.c_str());
        return nullptr;
    }

    auto* data = new ED();
    data->enc = std::move(enc);
    data->width = cfg.width;
    data->height = cfg.height;
    data->audioChannels = cfg.audioSampleRate > 0 ? cfg.audioChannels : 0;
    return data;
}

// JS: enc.addFrameRGBA(uint8Array [, stride])
//   Bytes must hold width*height*4 RGBA pixels, top-down.
JSValue js_videoEncoder_addFrameRGBA(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<ED>(ctx, this_val);
    if (!d || !d->enc) return JS_ThrowInternalError(ctx, "encoder closed");
    if (argc < 1) return JS_ThrowTypeError(ctx, "addFrameRGBA requires a Uint8Array");

    size_t byteLen = 0;
    size_t byteOff = 0;
    size_t bytesPerElem = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &byteOff, &byteLen, &bytesPerElem);
    if (JS_IsException(ab)) return ab;
    size_t bufSize = 0;
    uint8_t* buf = JS_GetArrayBuffer(ctx, &bufSize, ab);
    JS_FreeValue(ctx, ab);
    if (!buf) return JS_ThrowTypeError(ctx, "addFrameRGBA: argument is not a typed array");

    int stride = d->width * 4;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        int32_t s = stride;
        JS_ToInt32(ctx, &s, argv[1]);
        if (s > 0) stride = s;
    }
    const size_t needed = static_cast<size_t>(stride) * d->height;
    if (byteLen < needed) {
        return JS_ThrowRangeError(ctx,
            "addFrameRGBA: buffer too small (have %zu, need %zu)", byteLen, needed);
    }

    if (!d->enc->addFrameRGBA(buf + byteOff, stride)) {
        d->lastErr = d->enc->lastError();
        return JS_ThrowInternalError(ctx, "addFrameRGBA failed: %s", d->lastErr.c_str());
    }
    return JS_TRUE;
}

// JS: enc.addCanvasFrame(canvasElement)
//   Snapshots the canvas's Skia surface (preserves alpha) and encodes it.
//   Same pixel-source path as headless screenshotCanvas.
JSValue js_videoEncoder_addCanvasFrame(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<ED>(ctx, this_val);
    if (!d || !d->enc) return JS_ThrowInternalError(ctx, "encoder closed");
    if (argc < 1) return JS_ThrowTypeError(ctx, "addCanvasFrame requires a canvas element");

    auto* el = bro::js::getElement(argv[0]);
    if (!el) return JS_ThrowTypeError(ctx, "addCanvasFrame: argument is not an element");
    // A canvas hosting a 3D scene graph or WebGL context also carries an
    // auxiliary CanvasScene for ShapeNode/SpriteNode overlay compositing
    // (see draw_traversal.cpp), so canvasScene() can be non-null even when
    // it isn't the layer the app actually draws to — silently encoding it
    // would capture a blank/stale overlay instead of the real content.
    if (el->sceneGraph() || el->webglContext()) {
        return JS_ThrowTypeError(ctx,
            "addCanvasFrame: canvas has an active 3D scene or WebGL context, not a plain "
            "2D canvas — use addViewportFrame() to capture composited scene/WebGL content");
    }
    auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
    if (!cs) return JS_ThrowTypeError(ctx, "addCanvasFrame: element has no 2D canvas");

    cs->flush();
    auto* surf = cs->surface();
    if (!surf) return JS_ThrowInternalError(ctx, "addCanvasFrame: no surface");
    const int w = surf->width();
    const int h = surf->height();
    if (w != d->width || h != d->height) {
        return JS_ThrowRangeError(ctx,
            "addCanvasFrame: canvas %dx%d does not match encoder %dx%d",
            w, h, d->width, d->height);
    }
    auto pixels = cs->getImageData(0, 0, w, h);
    if (pixels.empty()) {
        return JS_ThrowInternalError(ctx, "addCanvasFrame: pixel read failed");
    }

    if (!d->enc->addFrameRGBA(pixels.data(), w * 4)) {
        d->lastErr = d->enc->lastError();
        return JS_ThrowInternalError(ctx, "addCanvasFrame: encode failed: %s",
                                     d->lastErr.c_str());
    }
    return JS_TRUE;
}

// JS: enc.addViewportFrame()
//   Captures the full composited viewport (canvas scene layers + HTML/CSS
//   overlay) — same pixel source as screenshot() — and encodes it. Use this
//   when the app draws part of its UI as DOM elements that addCanvasFrame
//   would miss. Viewport size must match encoder size.
JSValue js_videoEncoder_addViewportFrame(JSContext* ctx, JSValueConst this_val,
                                         int /*argc*/, JSValueConst* /*argv*/) {
    auto* d = qjsbind::unwrap<ED>(ctx, this_val);
    if (!d || !d->enc) return JS_ThrowInternalError(ctx, "encoder closed");

    auto* engine = getEngineForCtx(ctx);
    if (!engine) {
        return JS_ThrowInternalError(ctx, "addViewportFrame: engine not available");
    }
    const int w = engine->viewportWidth();
    const int h = engine->viewportHeight();
    if (w != d->width || h != d->height) {
        return JS_ThrowRangeError(ctx,
            "addViewportFrame: viewport %dx%d does not match encoder %dx%d",
            w, h, d->width, d->height);
    }
    auto pixels = engine->capturePixels();
    if (pixels.empty()) {
        return JS_ThrowInternalError(ctx, "addViewportFrame: pixel read failed");
    }
    if (!d->enc->addFrameRGBA(pixels.data(), w * 4)) {
        d->lastErr = d->enc->lastError();
        return JS_ThrowInternalError(ctx, "addViewportFrame: encode failed: %s",
                                     d->lastErr.c_str());
    }
    return JS_TRUE;
}

// JS: enc.addAudioFramesPCM(Float32Array)
//   Interleaved float PCM at the configured audioSampleRate / audioChannels.
//   Length must be a multiple of channelCount; values outside [-1,1] clip.
JSValue js_videoEncoder_addAudioFramesPCM(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<ED>(ctx, this_val);
    if (!d || !d->enc) return JS_ThrowInternalError(ctx, "encoder closed");
    if (argc < 1) return JS_ThrowTypeError(ctx, "addAudioFramesPCM requires a Float32Array");

    size_t byteLen = 0, byteOff = 0, bytesPerElem = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &byteOff, &byteLen, &bytesPerElem);
    if (JS_IsException(ab)) return ab;
    size_t bufSize = 0;
    uint8_t* buf = JS_GetArrayBuffer(ctx, &bufSize, ab);
    JS_FreeValue(ctx, ab);
    if (!buf || bytesPerElem != 4) {
        return JS_ThrowTypeError(ctx, "addAudioFramesPCM: expected Float32Array");
    }
    const float* pcm = reinterpret_cast<const float*>(buf + byteOff);
    const int totalSamples = static_cast<int>(byteLen / 4);
    if (d->audioChannels <= 0) {
        return JS_ThrowInternalError(ctx,
            "addAudioFramesPCM: encoder was not configured with audio");
    }
    if (totalSamples % d->audioChannels != 0) {
        return JS_ThrowRangeError(ctx,
            "addAudioFramesPCM: sample count (%d) not a multiple of channels (%d)",
            totalSamples, d->audioChannels);
    }
    const int frameCount = totalSamples / d->audioChannels;
    if (!d->enc->addAudioFramesPCM(pcm, frameCount)) {
        d->lastErr = d->enc->lastError();
        return JS_ThrowInternalError(ctx, "addAudioFramesPCM failed: %s",
                                     d->lastErr.c_str());
    }
    return JS_TRUE;
}

// JS: enc.finish() — flush + close. Idempotent. Returns true on success.
JSValue js_videoEncoder_finish(JSContext* ctx, JSValueConst this_val,
                               int /*argc*/, JSValueConst* /*argv*/) {
    auto* d = qjsbind::unwrap<ED>(ctx, this_val);
    if (!d) return JS_ThrowInternalError(ctx, "encoder gone");
    if (!d->enc) return JS_TRUE;
    bool ok = d->enc->finish();
    if (!ok) d->lastErr = d->enc->lastError();
    return JS_NewBool(ctx, ok);
}

// =========================================================================
// GifEncoder JS class — same shape as VideoEncoder but writes a GIF89a file.
// =========================================================================

struct GifEncoderData {
    std::unique_ptr<video::GifEncoder> enc;
    int width = 0;
    int height = 0;
    std::string lastErr;
};
using GD = GifEncoderData;

GD* js_gifEncoderCtor(JSContext* ctx, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        JS_ThrowTypeError(ctx, "GifEncoder requires a config object");
        return nullptr;
    }
    auto getInt = [&](const char* key, int dflt) -> int {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], key);
        int out = dflt;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            int32_t i = dflt;
            JS_ToInt32(ctx, &i, v);
            out = i;
        }
        JS_FreeValue(ctx, v);
        return out;
    };
    auto getStr = [&](const char* key) -> std::string {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], key);
        std::string out;
        if (JS_IsString(v)) {
            const char* s = JS_ToCString(ctx, v);
            if (s) { out = s; JS_FreeCString(ctx, s); }
        }
        JS_FreeValue(ctx, v);
        return out;
    };

    std::string path = getStr("path");
    if (path.empty()) {
        JS_ThrowTypeError(ctx, "GifEncoder: path is required");
        return nullptr;
    }
    video::GifEncoder::Config cfg;
    cfg.width = getInt("width", 0);
    cfg.height = getInt("height", 0);
    // Accept either fps (preferred) or delayCs. fps wins if both are set.
    const int fps = getInt("fps", 0);
    if (fps > 0) {
        cfg.delayCs = std::max(1, static_cast<int>(100.0 / fps + 0.5));
    } else {
        cfg.delayCs = getInt("delayCs", 4);
    }
    cfg.loopCount = getInt("loopCount", 0);
    cfg.paletteBits = getInt("paletteBits", 8);

    std::string err;
    auto enc = video::GifEncoder::create(path, cfg, &err);
    if (!enc) {
        JS_ThrowInternalError(ctx, "GifEncoder open failed: %s", err.c_str());
        return nullptr;
    }
    auto* data = new GD();
    data->enc = std::move(enc);
    data->width = cfg.width;
    data->height = cfg.height;
    return data;
}

JSValue js_gifEncoder_addFrameRGBA(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<GD>(ctx, this_val);
    if (!d || !d->enc) return JS_ThrowInternalError(ctx, "encoder closed");
    if (argc < 1) return JS_ThrowTypeError(ctx, "addFrameRGBA requires a Uint8Array");

    size_t byteLen = 0, byteOff = 0, bytesPerElem = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &byteOff, &byteLen, &bytesPerElem);
    if (JS_IsException(ab)) return ab;
    size_t bufSize = 0;
    uint8_t* buf = JS_GetArrayBuffer(ctx, &bufSize, ab);
    JS_FreeValue(ctx, ab);
    if (!buf) return JS_ThrowTypeError(ctx, "addFrameRGBA: argument is not a typed array");

    int stride = d->width * 4;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        int32_t s = stride;
        JS_ToInt32(ctx, &s, argv[1]);
        if (s > 0) stride = s;
    }
    const size_t needed = static_cast<size_t>(stride) * d->height;
    if (byteLen < needed) {
        return JS_ThrowRangeError(ctx,
            "addFrameRGBA: buffer too small (have %zu, need %zu)", byteLen, needed);
    }
    if (!d->enc->addFrameRGBA(buf + byteOff, stride)) {
        d->lastErr = d->enc->lastError();
        return JS_ThrowInternalError(ctx, "addFrameRGBA failed: %s", d->lastErr.c_str());
    }
    return JS_TRUE;
}

JSValue js_gifEncoder_addCanvasFrame(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<GD>(ctx, this_val);
    if (!d || !d->enc) return JS_ThrowInternalError(ctx, "encoder closed");
    if (argc < 1) return JS_ThrowTypeError(ctx, "addCanvasFrame requires a canvas element");

    auto* el = bro::js::getElement(argv[0]);
    if (!el) return JS_ThrowTypeError(ctx, "addCanvasFrame: argument is not an element");
    // See js_videoEncoder_addCanvasFrame: canvasScene() can be non-null even
    // for a 3D scene/WebGL canvas (auxiliary ShapeNode/SpriteNode overlay
    // layer), so it must not be used as a proxy for "this is a 2D canvas".
    if (el->sceneGraph() || el->webglContext()) {
        return JS_ThrowTypeError(ctx,
            "addCanvasFrame: canvas has an active 3D scene or WebGL context, not a plain "
            "2D canvas — capture scene/WebGL content via VideoEncoder.addViewportFrame() instead");
    }
    auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
    if (!cs) return JS_ThrowTypeError(ctx, "addCanvasFrame: element has no 2D canvas");

    cs->flush();
    auto* surf = cs->surface();
    if (!surf) return JS_ThrowInternalError(ctx, "addCanvasFrame: no surface");
    const int w = surf->width(), h = surf->height();
    if (w != d->width || h != d->height) {
        return JS_ThrowRangeError(ctx,
            "addCanvasFrame: canvas %dx%d does not match encoder %dx%d",
            w, h, d->width, d->height);
    }
    auto pixels = cs->getImageData(0, 0, w, h);
    if (pixels.empty()) return JS_ThrowInternalError(ctx, "addCanvasFrame: pixel read failed");
    if (!d->enc->addFrameRGBA(pixels.data(), w * 4)) {
        d->lastErr = d->enc->lastError();
        return JS_ThrowInternalError(ctx, "addCanvasFrame: encode failed: %s",
                                     d->lastErr.c_str());
    }
    return JS_TRUE;
}

// JS: gif.addViewportFrame()
//   The composited viewport, same pixel source as screenshot(). The webm
//   encoder has had this from the start and the gif encoder had not, which
//   made the two interchangeable from the addFrame side only for apps that
//   draw into a plain 2D canvas: addCanvasFrame REFUSES a canvas carrying a
//   3D scene or a WebGL context (its auxiliary CanvasScene would encode a
//   blank overlay), so "record this to a GIF" had no answer at all for a
//   WebGL app. It does now, and it is the same answer VideoEncoder gives.
JSValue js_gifEncoder_addViewportFrame(JSContext* ctx, JSValueConst this_val,
                                       int /*argc*/, JSValueConst* /*argv*/) {
    auto* d = qjsbind::unwrap<GD>(ctx, this_val);
    if (!d || !d->enc) return JS_ThrowInternalError(ctx, "encoder closed");

    auto* engine = getEngineForCtx(ctx);
    if (!engine) {
        return JS_ThrowInternalError(ctx, "addViewportFrame: engine not available");
    }
    const int w = engine->viewportWidth();
    const int h = engine->viewportHeight();
    if (w != d->width || h != d->height) {
        return JS_ThrowRangeError(ctx,
            "addViewportFrame: viewport %dx%d does not match encoder %dx%d",
            w, h, d->width, d->height);
    }
    auto pixels = engine->capturePixels();
    if (pixels.empty()) {
        return JS_ThrowInternalError(ctx, "addViewportFrame: pixel read failed");
    }
    if (!d->enc->addFrameRGBA(pixels.data(), w * 4)) {
        d->lastErr = d->enc->lastError();
        return JS_ThrowInternalError(ctx, "addViewportFrame: encode failed: %s",
                                     d->lastErr.c_str());
    }
    return JS_TRUE;
}

JSValue js_gifEncoder_setNextDelay(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<GD>(ctx, this_val);
    if (!d || !d->enc) return JS_ThrowInternalError(ctx, "encoder closed");
    if (argc < 1) return JS_UNDEFINED;
    int32_t delay = 0;
    JS_ToInt32(ctx, &delay, argv[0]);
    d->enc->setNextFrameDelayCs(delay);
    return JS_UNDEFINED;
}

JSValue js_gifEncoder_finish(JSContext* ctx, JSValueConst this_val,
                             int /*argc*/, JSValueConst* /*argv*/) {
    auto* d = qjsbind::unwrap<GD>(ctx, this_val);
    if (!d) return JS_ThrowInternalError(ctx, "encoder gone");
    if (!d->enc) return JS_TRUE;
    bool ok = d->enc->finish();
    if (!ok) d->lastErr = d->enc->lastError();
    return JS_NewBool(ctx, ok);
}

} // namespace


void VideoBindings::install(JSContext* ctx, const std::string& basePath)
{
        qjsbind::Class<ED>(ctx, "VideoEncoder")
            .constructor([](JSContext* ctx, int argc, JSValueConst* argv) -> ED* {
                return js_videoEncoderCtor(ctx, argc, argv);
            })
            .get("width",          [](ED* d) -> int { return d->width; })
            .get("height",         [](ED* d) -> int { return d->height; })
            .get("framesWritten",  [](ED* d) -> int {
                return d->enc ? d->enc->framesWritten() : 0;
            })
            .get("lastError",      [](ED* d) -> std::string {
                if (d->enc) return d->enc->lastError();
                return d->lastErr;
            })
            .method_raw("addFrameRGBA",       js_videoEncoder_addFrameRGBA, 1)
            .method_raw("addCanvasFrame",     js_videoEncoder_addCanvasFrame, 1)
            .method_raw("addViewportFrame",   js_videoEncoder_addViewportFrame, 0)
            .method_raw("addAudioFramesPCM",  js_videoEncoder_addAudioFramesPCM, 1)
            .method_raw("finish",             js_videoEncoder_finish, 0);
    
        qjsbind::Class<GD>(ctx, "GifEncoder")
            .constructor([](JSContext* ctx, int argc, JSValueConst* argv) -> GD* {
                return js_gifEncoderCtor(ctx, argc, argv);
            })
            .get("width",          [](GD* d) -> int { return d->width; })
            .get("height",         [](GD* d) -> int { return d->height; })
            .get("framesWritten",  [](GD* d) -> int {
                return d->enc ? d->enc->framesWritten() : 0;
            })
            .get("lastError",      [](GD* d) -> std::string {
                if (d->enc) return d->enc->lastError();
                return d->lastErr;
            })
            .method_raw("addFrameRGBA",       js_gifEncoder_addFrameRGBA, 1)
            .method_raw("addCanvasFrame",     js_gifEncoder_addCanvasFrame, 1)
            .method_raw("addViewportFrame",   js_gifEncoder_addViewportFrame, 0)
            .method_raw("setNextFrameDelayCs", js_gifEncoder_setNextDelay, 1)
            .method_raw("finish",             js_gifEncoder_finish, 0);
}


} // namespace bro::js

#endif // BRO_WITH_VIDEO
