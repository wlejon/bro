#include "bronze_host/host_eye.h"
#include "bronze_host/host_access.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_eye_taste.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_values.h"

#include "engine/engine.h"
#include "eye/eye_types.h"
#include "broimage/decode.h"

#if BRO_WITH_3D
#include "bronze_host/native_scene_internal.h"
#include "scene/scene_graph.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace bro::bronze_host::eye_detail {

using namespace bro::eye;

static std::string strProp(Value obj, const char* key, const std::string& def = {}) {
    if (!ev::isObject(obj)) return def;
    Value v = ev::getProperty(obj, key);
    return ev::isString(v) ? ev::toUtf8(v) : def;
}

static double numProp(Value obj, const char* key, double def) {
    if (!ev::isObject(obj)) return def;
    Value v = ev::getProperty(obj, key);
    if (v.isNumber()) {
        const double d = v.asNumber();
        return std::isfinite(d) ? d : def;
    }
    return def;
}

static bool boolProp(Value obj, const char* key, bool def) {
    if (!ev::isObject(obj)) return def;
    Value v = ev::getProperty(obj, key);
    return v.isBool() ? v.asBool() : def;
}

bool extractImage(Value v, EyeImage& out, bool allowLiveCapture, std::string& err) {
    out = EyeImage{};
    if (ev::isUndefined(v) || ev::isNull(v)) {
        if (!allowLiveCapture) {
            err = "expected an ImageData, ImageBitmap, SceneGraph, or image path";
            return false;
        }
        auto* eng = hostEngine();
        if (!eng) {
            err = "live capture requires main realm engine";
            return false;
        }
        eng->flush();
        auto px = eng->capturePixels();
        const int w = eng->framePixelWidth();
        const int h = eng->framePixelHeight();
        if (px.empty() || w <= 0 || h <= 0) {
            err = "engine produced empty frame";
            return false;
        }
        out.width = w;
        out.height = h;
        out.rgba = std::move(px);
        return true;
    }

    if (ev::isString(v)) {
        const std::string raw = ev::toUtf8(v);
        const std::string resolved = readableAssetPath(raw);
        broimage::Image decoded;
        std::string decErr;
        if (!broimage::decode_file(resolved, decoded, &decErr) || decoded.width <= 0 || decoded.height <= 0) {
            err = "could not decode image '" + raw + "': " + decErr;
            return false;
        }
        out.width = decoded.width;
        out.height = decoded.height;
        out.rgba = std::move(decoded.pixels);
        return true;
    }

    if (const auto* bmp = hostImageBitmapOf(v)) {
        const uint8_t* px = bmp->rgba();
        if (!px || bmp->width <= 0 || bmp->height <= 0) {
            err = "ImageBitmap is closed or empty";
            return false;
        }
        out.width = bmp->width;
        out.height = bmp->height;
        const size_t bytes = static_cast<size_t>(out.width) * out.height * 4;
        out.rgba.assign(px, px + bytes);
        return true;
    }

#if BRO_WITH_3D
    if (ev::isObject(v)) {
        if (void* raw = ev::handleData(v)) {
            auto* cell = static_cast<HostSceneGraphCell*>(raw);
            if (cell->tag == kHostSceneGraphTag) {
                auto* g = cell->graph();
                if (!g) {
                    err = "SceneGraph has been destroyed";
                    return false;
                }
                g->renderForRead();
                int w = 0, h = 0;
                auto px = g->readTonemapPixelsRGBA(w, h);
                if (px.empty() || w <= 0 || h <= 0) {
                    err = "SceneGraph produced no rendered pixels";
                    return false;
                }
                out.width = w;
                out.height = h;
                out.rgba = std::move(px);
                return true;
            }
        }
    }
#endif

    if (ev::isObject(v)) {
        ev::Persistent root(v);
        const int w = static_cast<int>(numProp(root.get(), "width", 0.0));
        const int h = static_cast<int>(numProp(root.get(), "height", 0.0));
        Value dataVal = ev::getProperty(root.get(), "data");
        if (w > 0 && h > 0 && ev::isObject(dataVal)) {
            if (auto info = ev::typedArrayInfo(dataVal); info && info.data) {
                const size_t expected = static_cast<size_t>(w) * h * 4;
                if (info.byteLength >= expected) {
                    out.width = w;
                    out.height = h;
                    out.rgba.assign(info.data, info.data + expected);
                    return true;
                }
            }
        }
    }

    err = "expected ImageData, ImageBitmap, SceneGraph, path string, or undefined (live frame)";
    return false;
}

EyeMeasureOptions parseMeasureOptions(Value optsVal) {
    EyeMeasureOptions o;
    if (!ev::isObject(optsVal)) return o;
    ev::Persistent r(optsVal);
    o.gridRows = std::clamp(static_cast<int>(numProp(r.get(), "rows", 3.0)), 1, 16);
    o.gridCols = std::clamp(static_cast<int>(numProp(r.get(), "cols", 3.0)), 1, 16);
    o.maxColors = std::clamp(static_cast<int>(numProp(r.get(), "maxColors", 5.0)), 0, 16);
    o.detectTiling = boolProp(r.get(), "tiling", true);
    return o;
}

EyeCompareOptions parseCompareOptions(Value optsVal) {
    EyeCompareOptions o;
    if (!ev::isObject(optsVal)) return o;
    ev::Persistent r(optsVal);
    o.gridRows = std::clamp(static_cast<int>(numProp(r.get(), "rows", 3.0)), 1, 16);
    o.gridCols = std::clamp(static_cast<int>(numProp(r.get(), "cols", 3.0)), 1, 16);
    o.changeThreshold = std::clamp(static_cast<int>(numProp(r.get(), "threshold", 8.0)), 0, 255);
    Value wv = ev::getProperty(r.get(), "weights");
    if (ev::isObject(wv)) {
        ev::Persistent wr(wv);
        o.ssimWeight = numProp(wr.get(), "ssim", o.ssimWeight);
        o.deltaEWeight = numProp(wr.get(), "deltaE", o.deltaEWeight);
        o.exposureWeight = numProp(wr.get(), "exposure", o.exposureWeight);
        o.colorWeight = numProp(wr.get(), "color", o.colorWeight);
    }
    return o;
}

EyeAuditOptions parseAuditOptions(Value optsVal) {
    EyeAuditOptions o;
    if (!ev::isObject(optsVal)) return o;
    ev::Persistent r(optsVal);
    o.selector = strProp(r.get(), "selector");
    o.minContrast = numProp(r.get(), "minContrast", o.minContrast);
    o.minApca = numProp(r.get(), "minApca", o.minApca);
    o.minHitTarget = numProp(r.get(), "minHitTarget", o.minHitTarget);
    o.minOverlapPx = numProp(r.get(), "minOverlapPx", o.minOverlapPx);
    o.checkOverlap = boolProp(r.get(), "overlap", o.checkOverlap);
    o.checkClip = boolProp(r.get(), "clip", o.checkClip);
    o.checkOffscreen = boolProp(r.get(), "offscreen", o.checkOffscreen);
    o.checkHitTargets = boolProp(r.get(), "hitTargets", o.checkHitTargets);
    o.checkFrame = boolProp(r.get(), "frame", o.checkFrame);
    o.maxIssues = std::clamp(static_cast<int>(numProp(r.get(), "maxIssues", 64.0)), 1, 512);
    return o;
}

EyeSceneOptions parseSceneOptions(Value optsVal) {
    EyeSceneOptions o;
    if (!ev::isObject(optsVal)) return o;
    ev::Persistent r(optsVal);
    o.focus = strProp(r.get(), "focus");
    o.render = boolProp(r.get(), "render", true);
    o.checkStability = boolProp(r.get(), "checkStability", false);
    o.checkFraming = boolProp(r.get(), "checkFraming", true);
    o.jitterPx = numProp(r.get(), "jitterPx", o.jitterPx);
    o.minSilhouetteContrast = numProp(r.get(), "minSilhouetteContrast", o.minSilhouetteContrast);
    o.floatMin = numProp(r.get(), "floatMin", o.floatMin);
    o.floatMax = numProp(r.get(), "floatMax", o.floatMax);
    o.penetrationEps = numProp(r.get(), "penetrationEps", o.penetrationEps);
    o.uvStretchThreshold = numProp(r.get(), "uvStretchThreshold", o.uvStretchThreshold);
    o.maxIssues = std::clamp(static_cast<int>(numProp(r.get(), "maxIssues", 64.0)), 1, 512);
    return o;
}

Value measurementToJs(const EyeMeasurement& m) {
    ObjectBuilder o;
    o.set("width", ev::fromDouble(m.width));
    o.set("height", ev::fromDouble(m.height));
    o.set("pixels", ev::fromDouble(static_cast<double>(m.pixels)));
    o.set("aspect", ev::fromDouble(m.aspect));

    o.set("lumaMean", ev::fromDouble(m.lumaMean));
    o.set("lumaStd", ev::fromDouble(m.lumaStd));
    o.set("lumaMin", ev::fromDouble(m.lumaMin));
    o.set("lumaMax", ev::fromDouble(m.lumaMax));
    o.set("lumaP05", ev::fromDouble(m.lumaP05));
    o.set("lumaP50", ev::fromDouble(m.lumaP50));
    o.set("lumaP95", ev::fromDouble(m.lumaP95));
    o.set("dynamicRange", ev::fromDouble(m.dynamicRange));
    o.set("exposureBias", ev::fromDouble(m.exposureBias));
    o.set("crushedBlackRatio", ev::fromDouble(m.crushedBlackRatio));
    o.set("blownWhiteRatio", ev::fromDouble(m.blownWhiteRatio));
    o.set("bandingScore", ev::fromDouble(m.bandingScore));
    o.set("zones", hostArrayOf(10, [&](size_t i) { return ev::fromDouble(m.zones[i]); }));

    o.set("chromaMean", ev::fromDouble(m.chromaMean));
    o.set("chromaStd", ev::fromDouble(m.chromaStd));
    o.set("chromaP95", ev::fromDouble(m.chromaP95));
    o.set("colorfulRatio", ev::fromDouble(m.colorfulRatio));
    o.set("hueEntropy", ev::fromDouble(m.hueEntropy));
    o.set("warmCoolBalance", ev::fromDouble(m.warmCoolBalance));

    o.set("dominantColors", hostArrayOf(m.dominantColors.size(), [&](size_t i) {
        const auto& c = m.dominantColors[i];
        ObjectBuilder dc;
        dc.set("r", ev::fromDouble(c.r));
        dc.set("g", ev::fromDouble(c.g));
        dc.set("b", ev::fromDouble(c.b));
        ev::Persistent hx(ev::fromUtf8(c.hex));
        dc.set("hex", hx.get());
        dc.set("L", ev::fromDouble(c.L));
        dc.set("C", ev::fromDouble(c.C));
        dc.set("h", ev::fromDouble(c.h));
        dc.set("share", ev::fromDouble(c.share));
        return dc.get();
    }));

    o.set("sharpness", ev::fromDouble(m.sharpness));
    o.set("laplacianVariance", ev::fromDouble(m.laplacianVariance));
    o.set("tenengrad", ev::fromDouble(m.tenengrad));
    o.set("edgeDensity", ev::fromDouble(m.edgeDensity));
    o.set("clutterScore", ev::fromDouble(m.clutterScore));
    o.set("whitespaceRatio", ev::fromDouble(m.whitespaceRatio));

    {
        ObjectBuilder t;
        t.set("score", ev::fromDouble(m.tiling.score));
        t.set("peakRatio", ev::fromDouble(m.tiling.peakRatio));
        t.set("periodX", ev::fromDouble(m.tiling.periodX));
        t.set("periodY", ev::fromDouble(m.tiling.periodY));
        t.set("horizontalBias", ev::fromDouble(m.tiling.horizontalBias));
        t.set("verticalBias", ev::fromDouble(m.tiling.verticalBias));
        o.set("tiling", t.get());
    }
    {
        ObjectBuilder sm;
        sm.set("score", ev::fromDouble(m.seam.score));
        sm.set("hSeam", ev::fromDouble(m.seam.hSeam));
        sm.set("vSeam", ev::fromDouble(m.seam.vSeam));
        sm.set("edgeDeltaE", ev::fromDouble(m.seam.edgeDeltaE));
        sm.set("interiorDeltaE", ev::fromDouble(m.seam.interiorDeltaE));
        o.set("seam", sm.get());
    }
    {
        ObjectBuilder db;
        db.set("low", ev::fromDouble(m.detailBands.low));
        db.set("mid", ev::fromDouble(m.detailBands.mid));
        db.set("high", ev::fromDouble(m.detailBands.high));
        o.set("detailBands", db.get());
    }
    {
        ObjectBuilder b;
        b.set("centerOfMassX", ev::fromDouble(m.balance.centerOfMassX));
        b.set("centerOfMassY", ev::fromDouble(m.balance.centerOfMassY));
        b.set("leftRightDiff", ev::fromDouble(m.balance.leftRightDiff));
        b.set("topBottomDiff", ev::fromDouble(m.balance.topBottomDiff));
        b.set("thirdsEnergy", ev::fromDouble(m.balance.thirdsEnergy));
        o.set("balance", b.get());
    }
    o.set("grid", hostArrayOf(m.grid.size(), [&](size_t i) {
        const auto& g = m.grid[i];
        ObjectBuilder gc;
        gc.set("x", ev::fromDouble(g.x));
        gc.set("y", ev::fromDouble(g.y));
        gc.set("w", ev::fromDouble(g.w));
        gc.set("h", ev::fromDouble(g.h));
        gc.set("lumaMean", ev::fromDouble(g.lumaMean));
        gc.set("chromaMean", ev::fromDouble(g.chromaMean));
        gc.set("edgeDensity", ev::fromDouble(g.edgeDensity));
        gc.set("crushedBlackRatio", ev::fromDouble(g.crushedBlackRatio));
        gc.set("blownWhiteRatio", ev::fromDouble(g.blownWhiteRatio));
        return gc.get();
    }));
    return o.get();
}

Value comparisonToJs(const EyeComparison& c) {
    ObjectBuilder o;
    o.set("score", ev::fromDouble(c.score));
    o.set("sameSize", ev::fromBool(c.sameSize));
    o.set("ssim", ev::fromDouble(c.ssim));
    o.set("ssimDist", ev::fromDouble(c.ssimDist));
    o.set("deltaEMean", ev::fromDouble(c.deltaEMean));
    o.set("deltaEP95", ev::fromDouble(c.deltaEP95));
    o.set("deltaEMax", ev::fromDouble(c.deltaEMax));
    o.set("changedRatio", ev::fromDouble(c.changedRatio));
    o.set("psnr", ev::fromDouble(c.psnr));
    o.set("rms", ev::fromDouble(c.rms));
    o.set("exposureDist", ev::fromDouble(c.exposureDist));
    o.set("colorDist", ev::fromDouble(c.colorDist));
    o.set("detailDist", ev::fromDouble(c.detailDist));
    o.set("lumaDiff", ev::fromDouble(c.lumaDiff));
    o.set("contrastRatio", ev::fromDouble(c.contrastRatio));
    o.set("chromaRatio", ev::fromDouble(c.chromaRatio));
    o.set("sharpnessRatio", ev::fromDouble(c.sharpnessRatio));
    o.set("tilingDiff", ev::fromDouble(c.tilingDiff));
    o.set("perceptual", perceptualDiffToJs(c.perceptual));
    o.set("cells", hostArrayOf(c.cells.size(), [&](size_t i) {
        const auto& cell = c.cells[i];
        ObjectBuilder cc;
        cc.set("x", ev::fromDouble(cell.x));
        cc.set("y", ev::fromDouble(cell.y));
        cc.set("w", ev::fromDouble(cell.w));
        cc.set("h", ev::fromDouble(cell.h));
        cc.set("ssim", ev::fromDouble(cell.ssim));
        cc.set("deltaEMean", ev::fromDouble(cell.deltaEMean));
        cc.set("lumaDiff", ev::fromDouble(cell.lumaDiff));
        cc.set("changedRatio", ev::fromDouble(cell.changedRatio));
        return cc.get();
    }));
    return o.get();
}

Value issueToJs(const EyeIssue& iss) {
    ObjectBuilder o;
    {
        ev::Persistent s(ev::fromUtf8(iss.kind));
        o.set("kind", s.get());
    }
    {
        ev::Persistent s(ev::fromUtf8(iss.severity));
        o.set("severity", s.get());
    }
    {
        ev::Persistent s(ev::fromUtf8(iss.selector));
        o.set("selector", s.get());
    }
    {
        ev::Persistent s(ev::fromUtf8(iss.message));
        o.set("message", s.get());
    }
    o.set("rect", makeHostRectValue(iss.rect.x, iss.rect.y, iss.rect.w, iss.rect.h));
    if (iss.hasContrast) {
        o.set("wcag", ev::fromDouble(iss.wcag));
        o.set("apca", ev::fromDouble(iss.apca));
        ev::Persistent fg(ev::fromUtf8(iss.fgHex));
        o.set("fg", fg.get());
        ev::Persistent bg(ev::fromUtf8(iss.bgHex));
        o.set("bg", bg.get());
        o.set("fontSize", ev::fromDouble(iss.fontSize));
        ev::Persistent tx(ev::fromUtf8(iss.text));
        o.set("text", tx.get());
    }
    if (iss.hasOverflow) {
        o.set("overflowX", ev::fromDouble(iss.overflowX));
        o.set("overflowY", ev::fromDouble(iss.overflowY));
    }
    if (iss.hasOverlap) {
        o.set("overlapRatio", ev::fromDouble(iss.overlapRatio));
        ev::Persistent os(ev::fromUtf8(iss.otherSelector));
        o.set("otherSelector", os.get());
    }
    if (iss.hasSpatial) {
        ev::Persistent nn(ev::fromUtf8(iss.nodeName));
        o.set("nodeName", nn.get());
        if (!iss.otherNodeName.empty()) {
            ev::Persistent on(ev::fromUtf8(iss.otherNodeName));
            o.set("otherNodeName", on.get());
        }
        o.set("gap", ev::fromDouble(iss.gap));
        o.set("value", ev::fromDouble(iss.value));
    }
    return o.get();
}

}  // namespace bro::bronze_host::eye_detail
