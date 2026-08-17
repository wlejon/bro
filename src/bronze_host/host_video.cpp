// VideoEncoder and GifEncoder: RGBA frames in, a .webm or .gif file out.
//
// The engine already owns both encoders (src/video/webm_encoder.h,
// src/video/gif_encoder.h) and bro's own JS binds them in
// src/js/video_bindings.cpp. So this file is a wrapper and deliberately
// nothing more: same class names, same method names, same argument shapes,
// same refusals. An app that records itself should not have to know which
// half of the runtime is executing it.
//
// THERE IS NO `VideoFrame` HERE because there is none in bro. The name comes
// from WebCodecs, whose model is a frame OBJECT you construct, hand to an
// encoder and then close; bro's encoders take pixels directly — a typed array,
// a 2D canvas, or the composited viewport — and own the copy. Adding a frame
// object to the compiled side alone would invent a surface the interpreted
// side does not have, which is the opposite of what this layer is for.
//
// WHY A COMPILED APP WANTS THIS AT ALL: capture is the one thing an app cannot
// do for itself. Everything else in this layer has a pure-JS fallback of some
// kind — an app could implement its own observer, its own parser, its own
// blob. It cannot implement VP9, and it cannot read the composited framebuffer
// without the engine handing it over.
//
// ---------------------------------------------------------------------------
// finish() IS NOT OPTIONAL HERE, and this is the one place this layer's
// behaviour genuinely differs from bro's JS.
//
// Both encoders finish from their destructor, so on the QuickJS side a program
// that forgets `finish()` still gets a complete file: the context teardown
// frees every object and the destructor runs. bronze has no teardown sweep —
// a handle's destructor runs from the post-collection hook of a collection
// that actually reclaims it (runtime/heap.cpp), and nothing collects at exit.
// So an encoder the program drops on the floor is finished only if a GC
// happens to reclaim it first, and otherwise the file keeps whatever the muxer
// had written and no trailer.
//
// The destructor still calls finish(), because the collection case is real and
// a half-written file is worse than a closed one. But the contract this layer
// documents is: call finish(). A truncated .webm is the failure, and it looks
// like a bug in the encoder rather than a missing call.
// ---------------------------------------------------------------------------
//
// PATHS ARE TAKEN AS GIVEN, resolved against the engine's working directory —
// what bro's encoders and screenshot() do, and NOT what host_image.cpp does
// with `src`. The asymmetry is the right one: an input is an asset that lives
// in the app directory, so resolving it there is the only thing that could be
// meant, while an output is a file the app is choosing to write and the app
// directory is usually the last place it wants it.

#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"  // ObjectBuilder, argAt

#if BRO_WITH_VIDEO

#include "canvas/canvas_scene.h"
#include "dom/element.h"
#include "engine/engine.h"
#include "video/gif_encoder.h"
#include "video/webm_encoder.h"

#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#endif  // BRO_WITH_VIDEO

namespace bro::bronze_host {

#if BRO_WITH_VIDEO

namespace {

// ---------------------------------------------------------------------------
// The two payloads
// ---------------------------------------------------------------------------

// Neither holds an ev::Persistent, which is what makes the destructors below
// legal: a handle finalizer runs mid-collection and may not call the embed API,
// and ~Persistent is the embed API (host_internal.h). Everything here is plain
// host memory plus a unique_ptr into src/video.
struct HostVideoEncoder {
    uint32_t tag = kHostVideoEncoderTag;  // must be first — see host_internal.h
    std::unique_ptr<video::WebmEncoder> enc;
    int width = 0;
    int height = 0;
    int audioChannels = 0;  // 0 = the file has no audio track
    std::string lastErr;
};

struct HostGifEncoder {
    uint32_t tag = kHostGifEncoderTag;  // must be first — see host_internal.h
    std::unique_ptr<video::GifEncoder> enc;
    int width = 0;
    int height = 0;
    std::string lastErr;
};

// Finishing from a finalizer is file I/O and a libvpx flush — slow for a
// collection, and worth it: the alternative is a file with no trailer. Neither
// touches a bronze value, which is the only thing a finalizer may not do.
void videoEncoderDtor(void* p) { delete static_cast<HostVideoEncoder*>(p); }
void gifEncoderDtor(void* p) { delete static_cast<HostGifEncoder*>(p); }

HostVideoEncoder* videoEncoderOf(Value v) {
    auto* d = static_cast<HostVideoEncoder*>(ev::handleData(v));
    if (!d || d->tag != kHostVideoEncoderTag) return nullptr;
    return d;
}

HostGifEncoder* gifEncoderOf(Value v) {
    auto* d = static_cast<HostGifEncoder*>(ev::handleData(v));
    if (!d || d->tag != kHostGifEncoderTag) return nullptr;
    return d;
}

// ---------------------------------------------------------------------------
// Reading the config object
// ---------------------------------------------------------------------------

// Every read here ALLOCATES — embed::getProperty builds the key string — so the
// config rides in a Persistent for the whole constructor and each read goes
// through its current address. A raw Value copy of the config would be stale
// from the second property onwards, which is the failure the GC rule at the top
// of host_internal.h exists to prevent, and it would present as a config whose
// later fields are all defaults.
struct ConfigReader {
    ev::Persistent cfg;

    explicit ConfigReader(Value v) : cfg(v) {}

    int getInt(const char* key, int dflt) {
        Value v = ev::getProperty(cfg.get(), key);
        // An absent key and an explicit undefined/null both mean "default",
        // which is what the JS binding does with the same three checks.
        if (ev::isUndefined(v) || ev::isNull(v)) return dflt;
        // An object here is a mistake (a nested config, a typo'd shape). Its
        // valueOf would answer NaN, and casting NaN to an int is undefined
        // behaviour rather than 0 — so it never reaches the cast.
        if (ev::isObject(v)) return dflt;
        const double d = ev::toDouble(v);
        if (std::isnan(d)) return dflt;
        return static_cast<int>(d);
    }

    std::string getStr(const char* key) {
        Value v = ev::getProperty(cfg.get(), key);
        if (ev::isUndefined(v) || ev::isNull(v) || ev::isObject(v)) return std::string();
        return ev::toUtf8(v);
    }
};

video::WebmEncoder::Quality parseQuality(const std::string& s) {
    if (s == "realtime") return video::WebmEncoder::Quality::Realtime;
    if (s == "best") return video::WebmEncoder::Quality::Best;
    return video::WebmEncoder::Quality::Good;  // the default, and "good"
}

// ---------------------------------------------------------------------------
// The two pixel sources that are not a typed array
// ---------------------------------------------------------------------------

// WHICH ERROR a refusal is, carried out alongside the message rather than
// folded into it. The KIND is API: `catch (e) { if (e.name === 'RangeError') …
// }` is how an app tells "I sized the encoder wrong" — recoverable, resize and
// retry — from "you handed me the wrong object", and bro's JS binding draws
// exactly this line (TypeError for the wrong thing, RangeError for the wrong
// size). A layer whose whole purpose is to be the same API as that one does not
// get to answer plain Error to both.
enum class Refusal { Type, Range, Internal };

Value throwRefusal(Refusal kind, const std::string& message) {
    switch (kind) {
        case Refusal::Type: return ev::throwTypeError(message);
        case Refusal::Range: return ev::throwRangeError(message);
        case Refusal::Internal: break;
    }
    return ev::throwError(message);
}

// A 2D canvas's pixels, or an empty vector with `err` and `kind` set. Shared by
// both encoders because the refusals are the interesting part and stating them
// twice is how the two drift.
//
// The 3D/WebGL refusal matters far more here than it does in bro's JS. A
// canvas hosting a scene graph or a WebGL context ALSO carries an auxiliary
// CanvasScene for overlay compositing (draw_traversal.cpp), so `canvasScene()`
// is non-null for it and reading that surface would silently encode a blank
// overlay instead of the picture. On the interpreted side that is a trap an app
// might never hit; a compiled app draws with WebGL as a matter of course, so
// this is the path it reaches for FIRST — and the message has to name the one
// that works rather than merely refuse.
std::vector<uint8_t> canvasPixels(Value elValue, int wantW, int wantH,
                                  std::string& err, Refusal& kind) {
    kind = Refusal::Type;
    dom::Element* el = hostElementOf(elValue);
    if (!el) {
        err = "addCanvasFrame: argument is not an element";
        return {};
    }
    if (el->sceneGraph() || el->webglContext()) {
        err = "addCanvasFrame: this canvas has a 3D scene or a WebGL context, not a "
              "plain 2D drawing buffer — use addViewportFrame() to capture "
              "composited scene/WebGL content";
        return {};
    }
    auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
    if (!cs) {
        err = "addCanvasFrame: this element has no 2D canvas";
        return {};
    }
    cs->flush();
    auto* surf = cs->surface();
    if (!surf) {
        kind = Refusal::Internal;
        err = "addCanvasFrame: the canvas has no surface yet";
        return {};
    }
    const int w = surf->width();
    const int h = surf->height();
    if (w != wantW || h != wantH) {
        kind = Refusal::Range;
        err = "addCanvasFrame: canvas " + std::to_string(w) + "x" + std::to_string(h) +
              " does not match encoder " + std::to_string(wantW) + "x" +
              std::to_string(wantH);
        return {};
    }
    auto pixels = cs->getImageData(0, 0, w, h);
    if (pixels.empty()) {
        kind = Refusal::Internal;
        err = "addCanvasFrame: pixel read failed";
    }
    return pixels;
}

// The composited viewport — canvas layers, WebGL, and the HTML/CSS overlay —
// which is the same pixel source headless screenshot() uses.
std::vector<uint8_t> viewportPixels(int wantW, int wantH, std::string& err,
                                    Refusal& kind) {
    kind = Refusal::Internal;
    engine::Engine* engine = hostEngine();
    if (!engine) {
        err = "addViewportFrame: no engine";
        return {};
    }
    const int w = engine->viewportWidth();
    const int h = engine->viewportHeight();
    if (w != wantW || h != wantH) {
        kind = Refusal::Range;
        err = "addViewportFrame: viewport " + std::to_string(w) + "x" + std::to_string(h) +
              " does not match encoder " + std::to_string(wantW) + "x" +
              std::to_string(wantH);
        return {};
    }
    auto pixels = engine->capturePixels();
    if (pixels.empty()) err = "addViewportFrame: pixel read failed";
    return pixels;
}

// ---------------------------------------------------------------------------
// The typed-array frame path, shared by both encoders
// ---------------------------------------------------------------------------

// THE POINTER ORDER, which is the whole reason this is a function rather than
// two copies: embed::typedArrayInfo answers a pointer INTO THE MOVING HEAP that
// dies at the next allocation (embed.h states it as loudly as it deserves). So
// the stride — whose argument may be an object whose valueOf allocates — is
// converted BEFORE the pointer is taken, the view rides in a Persistent so the
// conversion cannot leave a stale Value behind, and the encode call is the very
// next thing that happens after the read. The size check between them allocates
// nothing, and the throw on failure discards the pointer rather than using it.
//
// `add` is called with (bytes, stride) and returns the encoder's own bool.
template <typename AddFn>
Value addFrameFromTypedArray(std::span<const Value> a, int width, int height,
                             const char* what, AddFn&& add) {
    Value viewV = argAt(a, 0);
    Value strideV = argAt(a, 1);
    ev::Persistent view(viewV);

    int stride = width * 4;
    if (!ev::isUndefined(strideV) && !ev::isNull(strideV) && !ev::isObject(strideV)) {
        const double d = ev::toDouble(strideV);
        if (!std::isnan(d) && d > 0) stride = static_cast<int>(d);
    }

    const ev::TypedArrayInfo info = ev::typedArrayInfo(view.get());
    if (!info.data) {
        return ev::throwTypeError(std::string(what) + " expects a typed array of RGBA bytes");
    }
    const size_t needed = static_cast<size_t>(stride) * static_cast<size_t>(height);
    if (info.byteLength < needed) {
        return ev::throwRangeError(std::string(what) + ": buffer holds " +
                                   std::to_string(info.byteLength) + " bytes, needs " +
                                   std::to_string(needed));
    }
    // Immediately, while `info.data` is still an address that means something.
    return ev::fromBool(add(info.data, stride));
}

// ---------------------------------------------------------------------------
// VideoEncoder
// ---------------------------------------------------------------------------

Value makeVideoEncoderValue(HostVideoEncoder* payload) {
    ObjectBuilder b(ev::makeHandle(payload, videoEncoderDtor));

    // Accessors rather than data properties for all four: framesWritten and
    // lastError change with every frame, and width/height join them so the
    // read path is one kind of thing. A closed encoder answers its last known
    // values rather than throwing — a diagnostic that throws is one an error
    // handler cannot use.
    b.accessor("width", [](Value t, std::span<const Value>) {
        auto* d = videoEncoderOf(t);
        return ev::fromDouble(d ? d->width : 0);
    }, nullptr);
    b.accessor("height", [](Value t, std::span<const Value>) {
        auto* d = videoEncoderOf(t);
        return ev::fromDouble(d ? d->height : 0);
    }, nullptr);
    b.accessor("framesWritten", [](Value t, std::span<const Value>) {
        auto* d = videoEncoderOf(t);
        return ev::fromDouble(d && d->enc ? d->enc->framesWritten() : 0);
    }, nullptr);
    b.accessor("lastError", [](Value t, std::span<const Value>) {
        auto* d = videoEncoderOf(t);
        if (!d) return ev::fromUtf8(std::string());
        return ev::fromUtf8(d->enc ? d->enc->lastError() : d->lastErr);
    }, nullptr);

    b.def("addFrameRGBA", 2, [](Value thisValue, std::span<const Value> a) {
        ev::Persistent self(thisValue);
        HostVideoEncoder* d = videoEncoderOf(self.get());
        if (!d || !d->enc) return ev::throwError("VideoEncoder: the encoder is closed");
        if (d->width <= 0 || d->height <= 0) {
            // An audio-only file has no video track, and dropping frames into
            // it silently is how an app ends up with a soundtrack and no
            // picture and no idea why.
            return ev::throwError(
                "addFrameRGBA: this encoder was configured without a video track");
        }
        return addFrameFromTypedArray(
            a, d->width, d->height, "addFrameRGBA",
            [d](const uint8_t* px, int stride) {
                const bool ok = d->enc->addFrameRGBA(px, stride);
                if (!ok) d->lastErr = d->enc->lastError();
                return ok;
            });
    });

    b.def("addCanvasFrame", 1, [](Value thisValue, std::span<const Value> a) {
        ev::Persistent self(thisValue);
        Value elValue = argAt(a, 0);
        HostVideoEncoder* d = videoEncoderOf(self.get());
        if (!d || !d->enc) return ev::throwError("VideoEncoder: the encoder is closed");
        std::string err;
        Refusal kind = Refusal::Internal;
        // Host memory, so nothing below is racing the collector.
        std::vector<uint8_t> px = canvasPixels(elValue, d->width, d->height, err, kind);
        if (px.empty()) return throwRefusal(kind, err);
        if (!d->enc->addFrameRGBA(px.data(), d->width * 4)) {
            d->lastErr = d->enc->lastError();
            return ev::throwError("addCanvasFrame: encode failed: " + d->lastErr);
        }
        return ev::fromBool(true);
    });

    b.def("addViewportFrame", 0, [](Value thisValue, std::span<const Value>) {
        ev::Persistent self(thisValue);
        HostVideoEncoder* d = videoEncoderOf(self.get());
        if (!d || !d->enc) return ev::throwError("VideoEncoder: the encoder is closed");
        std::string err;
        Refusal kind = Refusal::Internal;
        std::vector<uint8_t> px = viewportPixels(d->width, d->height, err, kind);
        if (px.empty()) return throwRefusal(kind, err);
        if (!d->enc->addFrameRGBA(px.data(), d->width * 4)) {
            d->lastErr = d->enc->lastError();
            return ev::throwError("addViewportFrame: encode failed: " + d->lastErr);
        }
        return ev::fromBool(true);
    });

    b.def("addAudioFramesPCM", 1, [](Value thisValue, std::span<const Value> a) {
        ev::Persistent self(thisValue);
        Value pcmV = argAt(a, 0);
        ev::Persistent pcm(pcmV);
        HostVideoEncoder* d = videoEncoderOf(self.get());
        if (!d || !d->enc) return ev::throwError("VideoEncoder: the encoder is closed");
        if (d->audioChannels <= 0) {
            return ev::throwError(
                "addAudioFramesPCM: this encoder was configured without an audio track "
                "(set audioSampleRate)");
        }
        const ev::TypedArrayInfo info = ev::typedArrayInfo(pcm.get());
        // The element kind is checked rather than just the byte length: a
        // Uint8Array of the right size would otherwise be read as garbage
        // floats and encoded as noise, which is a bug you hear and cannot see.
        if (!info.data || info.elementKind != ev::elements::Float32) {
            return ev::throwTypeError("addAudioFramesPCM expects a Float32Array");
        }
        const int total = static_cast<int>(info.elementCount);
        if (total % d->audioChannels != 0) {
            return ev::throwRangeError("addAudioFramesPCM: " + std::to_string(total) +
                                       " samples is not a multiple of " +
                                       std::to_string(d->audioChannels) + " channels");
        }
        const bool ok = d->enc->addAudioFramesPCM(reinterpret_cast<const float*>(info.data),
                                                  total / d->audioChannels);
        if (!ok) d->lastErr = d->enc->lastError();
        return ev::fromBool(ok);
    });

    b.def("finish", 0, [](Value thisValue, std::span<const Value>) {
        ev::Persistent self(thisValue);
        HostVideoEncoder* d = videoEncoderOf(self.get());
        if (!d) return ev::throwError("VideoEncoder: not an encoder");
        // Idempotent, so a cleanup path that runs twice is not an error.
        if (!d->enc) return ev::fromBool(true);
        const bool ok = d->enc->finish();
        if (!ok) d->lastErr = d->enc->lastError();
        return ev::fromBool(ok);
    });

    return b.get();
}

Value videoEncoderCtor(Value, std::span<const Value> a) {
    Value cfgV = argAt(a, 0);
    if (!ev::isObject(cfgV)) {
        return ev::throwTypeError("VideoEncoder requires a config object");
    }
    ConfigReader r(cfgV);

    const std::string path = r.getStr("path");
    if (path.empty()) return ev::throwTypeError("VideoEncoder: `path` is required");

    video::WebmEncoder::Config cfg;
    cfg.width = r.getInt("width", 0);
    cfg.height = r.getInt("height", 0);
    cfg.fpsNum = r.getInt("fps", 30);
    cfg.fpsDen = r.getInt("fpsDen", 1);
    cfg.targetBitrateKbps = r.getInt("bitrateKbps", 0);
    cfg.keyframeIntervalSec = r.getInt("keyframeIntervalSec", 2);
    cfg.threads = r.getInt("threads", 0);
    cfg.rotationDegrees = r.getInt("rotation", 0);
    cfg.quality = parseQuality(r.getStr("quality"));
    cfg.audioSampleRate = r.getInt("audioSampleRate", 0);
    cfg.audioChannels = r.getInt("audioChannels", 2);
    cfg.audioBitrateKbps = r.getInt("audioBitrateKbps", 96);

    std::string err;
    auto enc = video::WebmEncoder::create(path, cfg, &err);
    if (!enc) return ev::throwError("VideoEncoder: could not open " + path + ": " + err);

    auto* d = new HostVideoEncoder();
    d->enc = std::move(enc);
    d->width = cfg.width;
    d->height = cfg.height;
    d->audioChannels = cfg.audioSampleRate > 0 ? cfg.audioChannels : 0;
    return makeVideoEncoderValue(d);
}

// ---------------------------------------------------------------------------
// GifEncoder
// ---------------------------------------------------------------------------

Value makeGifEncoderValue(HostGifEncoder* payload) {
    ObjectBuilder b(ev::makeHandle(payload, gifEncoderDtor));

    b.accessor("width", [](Value t, std::span<const Value>) {
        auto* d = gifEncoderOf(t);
        return ev::fromDouble(d ? d->width : 0);
    }, nullptr);
    b.accessor("height", [](Value t, std::span<const Value>) {
        auto* d = gifEncoderOf(t);
        return ev::fromDouble(d ? d->height : 0);
    }, nullptr);
    b.accessor("framesWritten", [](Value t, std::span<const Value>) {
        auto* d = gifEncoderOf(t);
        return ev::fromDouble(d && d->enc ? d->enc->framesWritten() : 0);
    }, nullptr);
    b.accessor("lastError", [](Value t, std::span<const Value>) {
        auto* d = gifEncoderOf(t);
        if (!d) return ev::fromUtf8(std::string());
        return ev::fromUtf8(d->enc ? d->enc->lastError() : d->lastErr);
    }, nullptr);

    b.def("addFrameRGBA", 2, [](Value thisValue, std::span<const Value> a) {
        ev::Persistent self(thisValue);
        HostGifEncoder* d = gifEncoderOf(self.get());
        if (!d || !d->enc) return ev::throwError("GifEncoder: the encoder is closed");
        return addFrameFromTypedArray(
            a, d->width, d->height, "addFrameRGBA",
            [d](const uint8_t* px, int stride) {
                const bool ok = d->enc->addFrameRGBA(px, stride);
                if (!ok) d->lastErr = d->enc->lastError();
                return ok;
            });
    });

    b.def("addCanvasFrame", 1, [](Value thisValue, std::span<const Value> a) {
        ev::Persistent self(thisValue);
        Value elValue = argAt(a, 0);
        HostGifEncoder* d = gifEncoderOf(self.get());
        if (!d || !d->enc) return ev::throwError("GifEncoder: the encoder is closed");
        std::string err;
        Refusal kind = Refusal::Internal;
        std::vector<uint8_t> px = canvasPixels(elValue, d->width, d->height, err, kind);
        if (px.empty()) return throwRefusal(kind, err);
        if (!d->enc->addFrameRGBA(px.data(), d->width * 4)) {
            d->lastErr = d->enc->lastError();
            return ev::throwError("addCanvasFrame: encode failed: " + d->lastErr);
        }
        return ev::fromBool(true);
    });

    // The same viewport capture VideoEncoder has. bro's JS GifEncoder does not
    // have it — it was added there in the same change as this file, because a
    // compiled app's picture is usually WebGL and addCanvasFrame refuses a
    // WebGL canvas by design. Without this, "record this to a GIF" is a request
    // this layer would have to answer with "only as WebM".
    b.def("addViewportFrame", 0, [](Value thisValue, std::span<const Value>) {
        ev::Persistent self(thisValue);
        HostGifEncoder* d = gifEncoderOf(self.get());
        if (!d || !d->enc) return ev::throwError("GifEncoder: the encoder is closed");
        std::string err;
        Refusal kind = Refusal::Internal;
        std::vector<uint8_t> px = viewportPixels(d->width, d->height, err, kind);
        if (px.empty()) return throwRefusal(kind, err);
        if (!d->enc->addFrameRGBA(px.data(), d->width * 4)) {
            d->lastErr = d->enc->lastError();
            return ev::throwError("addViewportFrame: encode failed: " + d->lastErr);
        }
        return ev::fromBool(true);
    });

    b.def("setNextFrameDelayCs", 1, [](Value thisValue, std::span<const Value> a) {
        ev::Persistent self(thisValue);
        Value delayV = argAt(a, 0);
        HostGifEncoder* d = gifEncoderOf(self.get());
        if (!d || !d->enc) return ev::throwError("GifEncoder: the encoder is closed");
        if (ev::isObject(delayV)) return ev::undefined();
        const double ms = ev::toDouble(delayV);
        if (!std::isnan(ms)) d->enc->setNextFrameDelayCs(static_cast<int>(ms));
        return ev::undefined();
    });

    b.def("finish", 0, [](Value thisValue, std::span<const Value>) {
        ev::Persistent self(thisValue);
        HostGifEncoder* d = gifEncoderOf(self.get());
        if (!d) return ev::throwError("GifEncoder: not an encoder");
        if (!d->enc) return ev::fromBool(true);
        const bool ok = d->enc->finish();
        if (!ok) d->lastErr = d->enc->lastError();
        return ev::fromBool(ok);
    });

    return b.get();
}

Value gifEncoderCtor(Value, std::span<const Value> a) {
    Value cfgV = argAt(a, 0);
    if (!ev::isObject(cfgV)) return ev::throwTypeError("GifEncoder requires a config object");
    ConfigReader r(cfgV);

    const std::string path = r.getStr("path");
    if (path.empty()) return ev::throwTypeError("GifEncoder: `path` is required");

    video::GifEncoder::Config cfg;
    cfg.width = r.getInt("width", 0);
    cfg.height = r.getInt("height", 0);
    // fps wins over delayCs when both are set, as it does in bro's JS: the two
    // say the same thing and fps is the one an app is more likely to mean.
    const int fps = r.getInt("fps", 0);
    if (fps > 0) {
        cfg.delayCs = static_cast<int>(100.0 / fps + 0.5);
        if (cfg.delayCs < 1) cfg.delayCs = 1;
    } else {
        cfg.delayCs = r.getInt("delayCs", 4);
    }
    cfg.loopCount = r.getInt("loopCount", 0);
    cfg.paletteBits = r.getInt("paletteBits", 8);

    std::string err;
    auto enc = video::GifEncoder::create(path, cfg, &err);
    if (!enc) return ev::throwError("GifEncoder: could not open " + path + ": " + err);

    auto* d = new HostGifEncoder();
    d->enc = std::move(enc);
    d->width = cfg.width;
    d->height = cfg.height;
    return makeGifEncoderValue(d);
}

}  // namespace

void installVideoGlobals() {
    // Reached through bronze_construct, which builds a plain instance, runs the
    // body with it as the receiver and then REPLACES it with whatever the body
    // returns — so the program gets the object built above, `VideoEncoder(cfg)`
    // without `new` does the same thing, and `enc instanceof VideoEncoder` is
    // false. host_image.cpp carries the full reasoning; it applies unchanged to
    // every constructor in this layer.
    ev::registerGlobal("VideoEncoder", ev::makeFunction(videoEncoderCtor, 1));
    ev::registerGlobal("GifEncoder", ev::makeFunction(gifEncoderCtor, 1));
}

#else  // !BRO_WITH_VIDEO

// ---------------------------------------------------------------------------
// The video-less build
// ---------------------------------------------------------------------------
//
// The names are still registered, bound to `undefined`. That is not a nicety —
// it is the only thing that works, and the rule generalises to every name in
// web_host.globals, so it is written here where the first compiled-out feature
// found it:
//
//   A NAME IN THE MANIFEST MUST BE REGISTERED IN EVERY BUILD, whatever value it
//   gets. Lowering admits manifest names as global reads; at run time
//   bronze_global_get asks the builtins, then the host registry, then
//   globalThis, and then calls fatal() (runtime/rt_state.cpp). A miss is not a
//   ReferenceError a program could catch and not an undefined it could test —
//   it aborts the process. So `typeof VideoEncoder` in a build without video
//   would kill an app whose only crime was asking whether it could record.
//
// Registered undefined is explicitly not a miss (runtime/host_globals.h says
// so), so the lookup succeeds and answers undefined — which makes
// `typeof VideoEncoder === 'undefined'` true, exactly the feature detection
// bro's own docs tell an app to write (docs/video-api.js), and exactly what an
// app sees on the interpreted side of a video-less build, where the classes are
// simply absent.
void installVideoGlobals() {
    ev::registerGlobal("VideoEncoder", ev::undefined());
    ev::registerGlobal("GifEncoder", ev::undefined());
}

#endif  // BRO_WITH_VIDEO

}  // namespace bro::bronze_host
