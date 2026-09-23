#include "bronze_host/host_canvas_pattern.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_globals_internal.h"
#include "canvas/canvas_scene.h"
#include "dom/element.h"
#include "engine/engine.h"
#include "layout/el_video.h"

#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>

#include <cmath>
#include <cstdlib>
#include <string>

namespace bro::bronze_host {

namespace {

HostClass g_canvasPatternClass;

void hostCanvasPatternDtor(void* p) {
    delete static_cast<HostCanvasPattern*>(p);
}

// DOMMatrix2DInit: a..f, with m11/m12/m21/m22/m41/m42 as the aliases. A
// member missing from the dictionary takes its identity default; when both
// spellings are present they must agree (the spec's "validate and fixup").
// Returns false (and throws) on a non-finite or contradictory member.
bool readMatrix2DInit(Value init, SkMatrix& out) {
    double v[6] = {1, 0, 0, 1, 0, 0};
    static const char* kShort[6] = {"a", "b", "c", "d", "e", "f"};
    static const char* kLong[6] = {"m11", "m12", "m21", "m22", "m41", "m42"};
    if (ev::isObject(init)) {
        for (int i = 0; i < 6; ++i) {
            // Read each member to a double straight away: getProperty
            // allocates, so no Value is held across the next call.
            const bool hasShort = !ev::isUndefined(ev::getProperty(init, kShort[i]));
            const double s = hasShort ? ev::toDouble(ev::getProperty(init, kShort[i])) : 0.0;
            const bool hasLong = !ev::isUndefined(ev::getProperty(init, kLong[i]));
            const double l = hasLong ? ev::toDouble(ev::getProperty(init, kLong[i])) : 0.0;
            if ((hasShort && !std::isfinite(s)) || (hasLong && !std::isfinite(l))) {
                ev::throwTypeError("CanvasPattern.setTransform: matrix members must be finite");
                return false;
            }
            if (hasShort && hasLong && s != l) {
                ev::throwTypeError(std::string("CanvasPattern.setTransform: '") + kShort[i] +
                                   "' and '" + kLong[i] + "' disagree");
                return false;
            }
            if (hasShort) v[i] = s;
            else if (hasLong) v[i] = l;
        }
    } else if (!ev::isUndefined(init) && !ev::isNull(init)) {
        ev::throwTypeError("CanvasPattern.setTransform: argument is not a DOMMatrix2DInit");
        return false;
    }
    out.setAll(static_cast<float>(v[0]), static_cast<float>(v[2]), static_cast<float>(v[4]),
               static_cast<float>(v[1]), static_cast<float>(v[3]), static_cast<float>(v[5]),
               0, 0, 1);
    return true;
}

void decorateCanvasPatternProto(ObjectBuilder& b) {
    b.def("setTransform", 0, [](Value self, std::span<const Value> a) {
        auto* p = hostCanvasPatternOf(self);
        if (!p || !p->data) return ev::undefined();
        SkMatrix m;
        if (!readMatrix2DInit(a.empty() ? ev::undefined() : a[0], m)) return ev::undefined();
        p->data->transform = m;
        return ev::undefined();
    });
}

void installCanvasPatternClass() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    g_canvasPatternClass.install("CanvasPattern", 0, nullptr, decorateCanvasPatternProto);
}

sk_sp<SkImage> imageFromRgba(const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return nullptr;
    auto info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    auto data = SkData::MakeWithCopy(rgba, static_cast<size_t>(w) * h * 4);
    return SkImages::RasterFromData(info, data, static_cast<size_t>(w) * 4);
}

// What createPattern's image argument resolves to. `bad` is the spec's
// "not usable yet" answer, which makes createPattern return null; a thrown
// error has already been raised when `threw` is set.
struct PatternSource {
    sk_sp<SkImage> image;
    bool bad = false;
    bool threw = false;
};

PatternSource resolvePatternSource(Value src) {
    PatternSource out;
    if (auto* bmp = hostImageBitmapOfMut(src)) {
        if (bmp->closed) {
            ev::throwValue(hostMakeDomError("InvalidStateError",
                "createPattern: the ImageBitmap has been closed or transferred"));
            out.threw = true;
            return out;
        }
        out.image = bmp->image ? bmp->image : imageFromRgba(bmp->pixels.data(), bmp->width, bmp->height);
        if (!out.image) out.bad = true;
        return out;
    }
    if (const HostImage* img = hostImageOf(src)) {
        if (!img->complete) { out.bad = true; return out; }
        if (!img->ok || img->rgba.empty()) {
            if (img->src.empty()) { out.bad = true; return out; }
            ev::throwValue(hostMakeDomError("InvalidStateError",
                "createPattern: the image is broken"));
            out.threw = true;
            return out;
        }
        out.image = imageFromRgba(img->rgba.data(), img->width, img->height);
        if (!out.image) out.bad = true;
        return out;
    }
    if (dom::Element* el = hostElementOf(src)) {
        std::string tag = el->tagName();
        for (char& c : tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (tag == "canvas") {
            const std::string& wa = el->getAttribute("width");
            const std::string& ha = el->getAttribute("height");
            if ((!wa.empty() && std::atoi(wa.c_str()) == 0) ||
                (!ha.empty() && std::atoi(ha.c_str()) == 0)) {
                ev::throwValue(hostMakeDomError("InvalidStateError",
                    "createPattern: the canvas has a zero width or height"));
                out.threw = true;
                return out;
            }
            if (!el->canvasScene()) {
                if (auto* eng = hostEngine()) eng->createCanvasContext(el);
            }
            // The snapshot is a raster copy, so the pattern keeps the canvas as
            // it was at createPattern time, as the spec requires, whatever is
            // drawn on the canvas afterwards.
            if (auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene())) {
                out.image = cs->snapshotImage();
            }
            if (!out.image) out.bad = true;
            return out;
        }
        // An <img> that has never been given a source carries no decoded
        // image at all (st->image is made on the first src): its request is
        // "unavailable", which is not usable yet — null, not a TypeError.
        if (tag == "img") {
            out.bad = true;
            return out;
        }
        if (auto* vc = el->videoControl()) {
            int vw = 0, vh = 0;
            const uint8_t* px = vc->currentFrameRgba(&vw, &vh);
            out.image = imageFromRgba(px, vw, vh);
            if (!out.image) out.bad = true;  // HAVE_NOTHING: no frame yet
            return out;
        }
    }
    ev::throwTypeError("createPattern: argument is not a CanvasImageSource "
                       "(HTMLImageElement, HTMLCanvasElement, HTMLVideoElement or ImageBitmap)");
    out.threw = true;
    return out;
}

}  // namespace

HostCanvasPattern* hostCanvasPatternOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostCanvasPattern*>(ev::handleData(v));
    if (!p || p->tag != 0x50415454u) return nullptr;
    return p;
}

Value createCanvasPatternValue(std::span<const Value> a) {
    installCanvasPatternClass();
    if (a.empty()) return ev::throwTypeError("createPattern requires an image argument");

    // Repetition first: a bad one is a SyntaxError even when the image would
    // have answered null. null and "" both mean "repeat".
    SkTileMode tx = SkTileMode::kRepeat, ty = SkTileMode::kRepeat;
    if (a.size() >= 2 && !ev::isNull(a[1]) && !ev::isUndefined(a[1])) {
        std::string rep = ev::toUtf8(a[1]);
        if (rep.empty() || rep == "repeat") {
        } else if (rep == "repeat-x") {
            ty = SkTileMode::kDecal;
        } else if (rep == "repeat-y") {
            tx = SkTileMode::kDecal;
        } else if (rep == "no-repeat") {
            tx = ty = SkTileMode::kDecal;
        } else {
            return ev::throwValue(hostMakeDomError("SyntaxError",
                "createPattern: '" + rep + "' is not a valid repetition"));
        }
    } else if (a.size() < 2) {
        return ev::throwTypeError("createPattern requires a repetition argument");
    }

    PatternSource s = resolvePatternSource(a[0]);
    if (s.threw) return ev::undefined();
    if (s.bad || !s.image) return ev::null();

    auto* p = new HostCanvasPattern();
    p->data = std::make_shared<canvas::CanvasPatternData>();
    p->data->image = std::move(s.image);
    p->data->tileX = tx;
    p->data->tileY = ty;
    return g_canvasPatternClass.make(p, hostCanvasPatternDtor);
}

}  // namespace bro::bronze_host
