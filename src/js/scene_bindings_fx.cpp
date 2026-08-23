#if BRO_WITH_3D

#include "js/scene_bindings.h"
#include "js/scene_bindings.h"
#include "js/scene_bindings_internal.h"
#include "js/runtime.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/shape_node.h"
#include "scene/sprite_node.h"
#include "scene/html_node.h"
#include "scene/particle_node.h"
#include "scene/particles3d_node.h"
#include "scene/gaussian_splat_node.h"
#include "scene/decal_node.h"
#include "util/log.h"
#include <qjsbind/qjsbind.h>
#include <bromesh/io/splat_ply.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bro::js {

// Forward decl — parseAnimSpec is defined further down (alongside the
// sprite-sheet parsing helpers), but js_sprite_addAnimation needs it.
static scene::SpriteNode::AnimationSpec parseAnimSpec(JSContext* ctx, JSValueConst obj);

// Parse a `worldAnchor: [x, y, z]` or `{x, y, z}` option into a Vec3.
// Returns true if the option was present (even if partially specified).
static bool parseWorldAnchor(JSContext* ctx, JSValueConst opts, bromath::Vec3& out) {
    JSValue v = JS_GetPropertyStr(ctx, opts, "worldAnchor");
    bool found = false;
    if (JS_IsArray(v)) {
        found = true;
        JSValue e0 = JS_GetPropertyUint32(ctx, v, 0);
        JSValue e1 = JS_GetPropertyUint32(ctx, v, 1);
        JSValue e2 = JS_GetPropertyUint32(ctx, v, 2);
        double x = 0, y = 0, z = 0;
        JS_ToFloat64(ctx, &x, e0);
        JS_ToFloat64(ctx, &y, e1);
        JS_ToFloat64(ctx, &z, e2);
        out = {(float)x, (float)y, (float)z};
        JS_FreeValue(ctx, e0);
        JS_FreeValue(ctx, e1);
        JS_FreeValue(ctx, e2);
    } else if (JS_IsObject(v)) {
        found = true;
        double x = 0, y = 0, z = 0;
        JSValue ex = JS_GetPropertyStr(ctx, v, "x");
        JSValue ey = JS_GetPropertyStr(ctx, v, "y");
        JSValue ez = JS_GetPropertyStr(ctx, v, "z");
        if (!JS_IsUndefined(ex)) JS_ToFloat64(ctx, &x, ex);
        if (!JS_IsUndefined(ey)) JS_ToFloat64(ctx, &y, ey);
        if (!JS_IsUndefined(ez)) JS_ToFloat64(ctx, &z, ez);
        out = {(float)x, (float)y, (float)z};
        JS_FreeValue(ctx, ex);
        JS_FreeValue(ctx, ey);
        JS_FreeValue(ctx, ez);
    }
    JS_FreeValue(ctx, v);
    return found;
}

// Apply worldAnchor + billboard options to any SceneNode. Safe to call on
// nodes that don't support billboarding — the fields are harmless.
static void applyBillboardOpts(JSContext* ctx, JSValueConst opts, scene::SceneNode* node) {
    if (!node) return;

    bromath::Vec3 anchor;
    if (parseWorldAnchor(ctx, opts, anchor)) {
        node->setWorldAnchor(anchor);
    }

    JSValue bbVal = JS_GetPropertyStr(ctx, opts, "billboard");
    if (JS_IsString(bbVal)) {
        std::string mode = jsStr(ctx, bbVal);
        if (mode == "ylock" || mode == "yLock" || mode == "y-lock") {
            node->setBillboardMode(scene::SceneNode::BillboardMode::YLock);
        } else {
            node->setBillboardMode(scene::SceneNode::BillboardMode::Full);
        }
    }
    JS_FreeValue(ctx, bbVal);
}

// setHtml(htmlString) — HtmlNode only
JSValue js_node_setHtml(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node() || argc < 1) return JS_UNDEFINED;
    if (w->node()->type() != scene::SceneNode::Type::Html)
        return JS_ThrowTypeError(ctx, "setHtml: node is not an HtmlNode");
    auto* hn = static_cast<scene::HtmlNode*>(w->node());
    hn->setHtml(jsStr(ctx, argv[0]));
    return JS_UNDEFINED;
}

// markHtmlDirty() — HtmlNode only; force a re-raster on the next frame.
// Useful after imperative DOM mutation via node.root.
JSValue js_node_markHtmlDirty(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node()) return JS_UNDEFINED;
    if (w->node()->type() != scene::SceneNode::Type::Html)
        return JS_ThrowTypeError(ctx, "markHtmlDirty: node is not an HtmlNode");
    static_cast<scene::HtmlNode*>(w->node())->markHtmlDirty();
    return JS_UNDEFINED;
}

// savePly(path) — GaussianSplat only. Writes the node's splat cloud to a
// 3D-Gaussian-Splat .ply (the INRIA/3DGS field convention bromesh emits), so a
// reconstructed cloud can be saved and reopened (here or in any splat viewer).
// `path` is resolved like createGaussianSplat's: absolute/drive paths pass
// through, leading-slash consults mounts, else app-relative. Returns true on
// success.
JSValue js_node_savePly(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node() || argc < 1)
        return JS_ThrowTypeError(ctx, "savePly(path): path required");
    if (w->node()->type() != scene::SceneNode::Type::GaussianSplat)
        return JS_ThrowTypeError(ctx, "savePly: node is not a GaussianSplat");
    auto* sn = static_cast<scene::GaussianSplatNode*>(w->node());
    if (sn->splatCount() == 0)
        return JS_ThrowTypeError(ctx, "savePly: splat cloud is empty");
    std::string path = resolveAppPath(jsStr(ctx, argv[0]));
    bool ok = bromesh::saveSplatPLY(sn->cloud(), path);
    if (!ok)
        return JS_ThrowInternalError(ctx, "savePly: failed to write '%s'", path.c_str());
    LOG_INFO("savePly: wrote %zu splats (SH deg %d) to '%s'",
             sn->cloud().count(), sn->cloud().shDegree, path.c_str());
    return JS_NewBool(ctx, 1);
}

// ---------------------------------------------------------------------------
// Sprite animation-end JS callback. The JSFnRef is owned by the std::function
// stored on the SpriteNode, so the JS ref is released on every destruction
// path — direct destroy, ancestor subtree destroy, graph teardown — with no
// side registry to sweep. (This replaced a process-global map keyed by node
// id that leaked its entries on subtree destroy and graph prune.)
// ---------------------------------------------------------------------------

void installSpriteEndCallback(scene::SpriteNode* node, JSContext* ctx, JSValue fn) {
    if (JS_IsFunction(ctx, fn)) {
        auto ref = std::make_shared<JSFnRef>(ctx, JS_DupValue(ctx, fn));
        node->setOnAnimationEnd([ref](const std::string& name) {
            // Copy everything needed to locals up front: the JS callback may
            // destroy the sprite node, which destroys this std::function (and
            // `ref` with it) while we are still executing.
            JSContext* c = ref->ctx;
            JSValue dup = JS_DupValue(c, ref->fn);
            JSValue arg = JS_NewString(c, name.c_str());
            JSValue ret = JS_Call(c, dup, JS_UNDEFINED, 1, &arg);
            if (JS_IsException(ret)) {
                Runtime::checkException(c, ret);
            }
            JS_FreeValue(c, ret);
            JS_FreeValue(c, arg);
            JS_FreeValue(c, dup);
        });
    } else {
        node->setOnAnimationEnd(nullptr);
    }
}

// ---------------------------------------------------------------------------
// Sprite node methods
// ---------------------------------------------------------------------------

JSValue js_sprite_addAnimation(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node() || w->node()->type() != scene::SceneNode::Type::Sprite || argc < 2)
        return JS_UNDEFINED;
    auto* s = static_cast<scene::SpriteNode*>(w->node());
    std::string name = jsStr(ctx, argv[0]);
    if (JS_IsObject(argv[1])) {
        s->addAnimation(name, parseAnimSpec(ctx, argv[1]));
    }
    return JS_DupValue(ctx, this_val);
}

// ---------------------------------------------------------------------------
// Particle node helpers + methods
// ---------------------------------------------------------------------------

static bromath::Color colorFromJS(JSContext* ctx, JSValueConst v, bromath::Color def) {
    if (JS_IsString(v)) {
        uint8_t r, g, b, a;
        std::string s = jsStr(ctx, v);
        if (parseColor(s, r, g, b, a)) return bromath::cfromColor8({r, g, b, a});
    }
    return def;
}

// Read a property that can be a number or {min,max}; returns chosen min,max.
static void parseRange(JSContext* ctx, JSValueConst opts, const char* key,
                       float& outMin, float& outMax) {
    JSValue v = JS_GetPropertyStr(ctx, opts, key);
    if (JS_IsNumber(v)) {
        double n = 0; JS_ToFloat64(ctx, &n, v);
        outMin = outMax = (float)n;
    } else if (JS_IsObject(v)) {
        outMin = (float)qjsbind::get_prop_number(ctx, v, "min", outMin);
        outMax = (float)qjsbind::get_prop_number(ctx, v, "max", outMax);
    }
    JS_FreeValue(ctx, v);
}

static void applyParticleOpts(JSContext* ctx, JSValueConst opts, scene::ParticleNode* node) {
    // maxParticles (must run before any burst/play)
    JSValue mpVal = JS_GetPropertyStr(ctx, opts, "maxParticles");
    if (JS_IsNumber(mpVal)) {
        int32_t n = 256; JS_ToInt32(ctx, &n, mpVal);
        node->setMaxParticles(n);
    }
    JS_FreeValue(ctx, mpVal);

    // texture
    JSValue texVal = JS_GetPropertyStr(ctx, opts, "texture");
    if (JS_IsString(texVal)) node->setTexturePath(jsStr(ctx, texVal));
    JS_FreeValue(ctx, texVal);

    // blend
    JSValue blendVal = JS_GetPropertyStr(ctx, opts, "blend");
    if (JS_IsString(blendVal)) {
        std::string s = jsStr(ctx, blendVal);
        node->setBlend(s == "additive" ? scene::ParticleNode::Blend::Additive
                                       : scene::ParticleNode::Blend::Normal);
    }
    JS_FreeValue(ctx, blendVal);

    // rate
    JSValue rateVal = JS_GetPropertyStr(ctx, opts, "rate");
    if (JS_IsNumber(rateVal)) node->setRate((float)jsNum(ctx, rateVal));
    JS_FreeValue(ctx, rateVal);

    // lifetime
    {
        float lo = 0.5f, hi = 1.0f;
        parseRange(ctx, opts, "lifetime", lo, hi);
        node->setLifetime(lo, hi);
    }

    // velocity: { angle, angleSpread, speed, speedSpread }
    JSValue velVal = JS_GetPropertyStr(ctx, opts, "velocity");
    if (JS_IsObject(velVal)) {
        node->setVelocity(
            (float)qjsbind::get_prop_number(ctx, velVal, "angle", -90),
            (float)qjsbind::get_prop_number(ctx, velVal, "angleSpread", 360),
            (float)qjsbind::get_prop_number(ctx, velVal, "speed", 100),
            (float)qjsbind::get_prop_number(ctx, velVal, "speedSpread", 0));
    }
    JS_FreeValue(ctx, velVal);

    // gravity: { x, y } or [x, y]
    JSValue gravVal = JS_GetPropertyStr(ctx, opts, "gravity");
    if (JS_IsObject(gravVal)) {
        if (JS_IsArray(gravVal)) {
            JSValue gx = JS_GetPropertyUint32(ctx, gravVal, 0);
            JSValue gy = JS_GetPropertyUint32(ctx, gravVal, 1);
            node->setGravity((float)jsNum(ctx, gx), (float)jsNum(ctx, gy));
            JS_FreeValue(ctx, gx); JS_FreeValue(ctx, gy);
        } else {
            node->setGravity(
                (float)qjsbind::get_prop_number(ctx, gravVal, "x", 0),
                (float)qjsbind::get_prop_number(ctx, gravVal, "y", 0));
        }
    }
    JS_FreeValue(ctx, gravVal);

    // size: { start, end }
    JSValue sizeVal = JS_GetPropertyStr(ctx, opts, "size");
    if (JS_IsObject(sizeVal)) {
        node->setSize(
            (float)qjsbind::get_prop_number(ctx, sizeVal, "start", 6),
            (float)qjsbind::get_prop_number(ctx, sizeVal, "end", 0));
    } else if (JS_IsNumber(sizeVal)) {
        float v = (float)jsNum(ctx, sizeVal);
        node->setSize(v, v);
    }
    JS_FreeValue(ctx, sizeVal);

    // color: { start, end }
    JSValue colorVal = JS_GetPropertyStr(ctx, opts, "color");
    if (JS_IsObject(colorVal)) {
        JSValue cs = JS_GetPropertyStr(ctx, colorVal, "start");
        JSValue ce = JS_GetPropertyStr(ctx, colorVal, "end");
        bromath::Color start = colorFromJS(ctx, cs, bromath::cfromColor8({255,255,255,255}));
        bromath::Color end   = colorFromJS(ctx, ce, bromath::Color{start.r, start.g, start.b, 0.0f});
        node->setColors(start, end);
        JS_FreeValue(ctx, cs); JS_FreeValue(ctx, ce);
    } else if (JS_IsString(colorVal)) {
        bromath::Color c = colorFromJS(ctx, colorVal, bromath::cfromColor8({255,255,255,255}));
        bromath::Color end = c; end.a = 0.0f;
        node->setColors(c, end);
    }
    JS_FreeValue(ctx, colorVal);

    // rotation: { start, spinSpeed, spinSpread }
    JSValue rotVal = JS_GetPropertyStr(ctx, opts, "rotation");
    if (JS_IsObject(rotVal)) {
        node->setRotation(
            (float)qjsbind::get_prop_number(ctx, rotVal, "start", 0),
            (float)qjsbind::get_prop_number(ctx, rotVal, "spinSpeed", 0),
            (float)qjsbind::get_prop_number(ctx, rotVal, "spinSpread", 0));
    }
    JS_FreeValue(ctx, rotVal);

    // drag (per-second multiplier)
    JSValue dragVal = JS_GetPropertyStr(ctx, opts, "drag");
    if (JS_IsNumber(dragVal)) node->setDrag((float)jsNum(ctx, dragVal));
    JS_FreeValue(ctx, dragVal);

    // initial burst
    JSValue burstVal = JS_GetPropertyStr(ctx, opts, "burst");
    if (JS_IsNumber(burstVal)) {
        int32_t n = 0; JS_ToInt32(ctx, &n, burstVal);
        node->burst(n);
    }
    JS_FreeValue(ctx, burstVal);

    // autoplay (default true)
    JSValue apVal = JS_GetPropertyStr(ctx, opts, "autoplay");
    bool autoplay = JS_IsUndefined(apVal) ? true : JS_ToBool(ctx, apVal);
    JS_FreeValue(ctx, apVal);
    if (autoplay) node->play(); else node->stop();
}

// ---------------------------------------------------------------------------
// 3D particle node helpers + methods
// ---------------------------------------------------------------------------

// Read a [x,y,z] array or {x,y,z} object property into a Vec3. Returns false
// (out untouched) when the property is absent or malformed.
static bool parseVec3Prop(JSContext* ctx, JSValueConst opts, const char* key,
                          bromath::Vec3& out) {
    JSValue v = JS_GetPropertyStr(ctx, opts, key);
    bool ok = false;
    if (JS_IsArray(v)) {
        JSValue e0 = JS_GetPropertyUint32(ctx, v, 0);
        JSValue e1 = JS_GetPropertyUint32(ctx, v, 1);
        JSValue e2 = JS_GetPropertyUint32(ctx, v, 2);
        double x = 0, y = 0, z = 0;
        JS_ToFloat64(ctx, &x, e0);
        JS_ToFloat64(ctx, &y, e1);
        JS_ToFloat64(ctx, &z, e2);
        JS_FreeValue(ctx, e0); JS_FreeValue(ctx, e1); JS_FreeValue(ctx, e2);
        out = {(float)x, (float)y, (float)z};
        ok = true;
    } else if (JS_IsObject(v)) {
        out = {(float)qjsbind::get_prop_number(ctx, v, "x", 0),
               (float)qjsbind::get_prop_number(ctx, v, "y", 0),
               (float)qjsbind::get_prop_number(ctx, v, "z", 0)};
        ok = true;
    }
    JS_FreeValue(ctx, v);
    return ok;
}

void installParticles3DOnFinished(JSContext* ctx, JSValueConst fnVal,
                                         scene::Particles3DNode* node) {
    if (JS_IsFunction(ctx, fnVal)) {
        auto ref = std::make_shared<JSFnRef>(ctx, JS_DupValue(ctx, fnVal));
        node->setOnFinished([ref]() {
            JSValue fn = JS_DupValue(ref->ctx, ref->fn);
            JSValue r = JS_Call(ref->ctx, fn, JS_UNDEFINED, 0, nullptr);
            if (JS_IsException(r)) Runtime::checkException(ref->ctx, r);
            JS_FreeValue(ref->ctx, r);
            JS_FreeValue(ref->ctx, fn);
        });
    } else {
        node->setOnFinished(nullptr);
    }
}

static scene::Particles3DNode::EmitterShape particleShapeFromString(const std::string& s) {
    using ES = scene::Particles3DNode::EmitterShape;
    if (s == "sphere")     return ES::Sphere;
    if (s == "hemisphere") return ES::Hemisphere;
    if (s == "box")        return ES::Box;
    if (s == "cone")       return ES::Cone;
    return ES::Point;
}

static void applyParticle3DOpts(JSContext* ctx, JSValueConst opts,
                                scene::Particles3DNode* node) {
    // maxParticles (must run before any burst/play — reallocates the pool)
    JSValue mpVal = JS_GetPropertyStr(ctx, opts, "maxParticles");
    if (JS_IsNumber(mpVal)) {
        int32_t n = 256; JS_ToInt32(ctx, &n, mpVal);
        node->setMaxParticles(n);
    }
    JS_FreeValue(ctx, mpVal);

    // seed (reseed before any emission so bursts are deterministic too)
    JSValue seedVal = JS_GetPropertyStr(ctx, opts, "seed");
    if (JS_IsNumber(seedVal)) {
        double s = 0; JS_ToFloat64(ctx, &s, seedVal);
        node->setSeed(static_cast<uint64_t>(s));
    }
    JS_FreeValue(ctx, seedVal);

    // texture (+ optional flipbook sheet {cols, rows, frames})
    JSValue texVal = JS_GetPropertyStr(ctx, opts, "texture");
    if (JS_IsString(texVal)) node->setTexturePath(resolveAppPath(jsStr(ctx, texVal)));
    JS_FreeValue(ctx, texVal);

    JSValue sheetVal = JS_GetPropertyStr(ctx, opts, "sheet");
    if (JS_IsObject(sheetVal)) {
        node->setSheet(
            (int)qjsbind::get_prop_number(ctx, sheetVal, "cols", 1),
            (int)qjsbind::get_prop_number(ctx, sheetVal, "rows", 1),
            (int)qjsbind::get_prop_number(ctx, sheetVal, "frames", 0));
    }
    JS_FreeValue(ctx, sheetVal);

    // blend
    JSValue blendVal = JS_GetPropertyStr(ctx, opts, "blend");
    if (JS_IsString(blendVal)) {
        std::string s = jsStr(ctx, blendVal);
        node->setBlend(s == "additive" ? scene::Particles3DNode::Blend::Additive
                                       : scene::Particles3DNode::Blend::Normal);
    }
    JS_FreeValue(ctx, blendVal);

    // emitter shape: "sphere" | {type:"cone", radius, angle, extents:[x,y,z]}
    JSValue shapeVal = JS_GetPropertyStr(ctx, opts, "shape");
    if (JS_IsString(shapeVal)) {
        node->setShape(particleShapeFromString(jsStr(ctx, shapeVal)));
    } else if (JS_IsObject(shapeVal)) {
        node->setShape(particleShapeFromString(
            qjsbind::get_prop_string(ctx, shapeVal, "type", "point")));
        JSValue rVal = JS_GetPropertyStr(ctx, shapeVal, "radius");
        if (JS_IsNumber(rVal)) node->setShapeRadius((float)jsNum(ctx, rVal));
        JS_FreeValue(ctx, rVal);
        JSValue aVal = JS_GetPropertyStr(ctx, shapeVal, "angle");
        if (JS_IsNumber(aVal)) node->setConeAngle((float)jsNum(ctx, aVal));
        JS_FreeValue(ctx, aVal);
        bromath::Vec3 he;
        if (parseVec3Prop(ctx, shapeVal, "extents", he)) node->setShapeExtents(he);
    }
    JS_FreeValue(ctx, shapeVal);

    // simulation space
    JSValue spaceVal = JS_GetPropertyStr(ctx, opts, "space");
    if (JS_IsString(spaceVal)) {
        node->setSpace(jsStr(ctx, spaceVal) == "local"
                           ? scene::Particles3DNode::SimSpace::Local
                           : scene::Particles3DNode::SimSpace::World);
    }
    JS_FreeValue(ctx, spaceVal);

    // rate
    JSValue rateVal = JS_GetPropertyStr(ctx, opts, "rate");
    if (JS_IsNumber(rateVal)) node->setRate((float)jsNum(ctx, rateVal));
    JS_FreeValue(ctx, rateVal);

    // lifetime
    {
        float lo = 0.5f, hi = 1.0f;
        parseRange(ctx, opts, "lifetime", lo, hi);
        node->setLifetime(lo, hi);
    }

    // velocity: { direction:[x,y,z], spread(deg), speed, speedSpread }
    JSValue velVal = JS_GetPropertyStr(ctx, opts, "velocity");
    if (JS_IsObject(velVal)) {
        bromath::Vec3 dir{0.0f, 1.0f, 0.0f};
        parseVec3Prop(ctx, velVal, "direction", dir);
        node->setDirection(dir,
            (float)qjsbind::get_prop_number(ctx, velVal, "spread", 0));
        node->setSpeed(
            (float)qjsbind::get_prop_number(ctx, velVal, "speed", 1),
            (float)qjsbind::get_prop_number(ctx, velVal, "speedSpread", 0));
    }
    JS_FreeValue(ctx, velVal);

    // gravity: [x,y,z] or {x,y,z}
    {
        bromath::Vec3 g;
        if (parseVec3Prop(ctx, opts, "gravity", g)) node->setGravity(g);
    }

    // size: { start, end } or number (world units)
    JSValue sizeVal = JS_GetPropertyStr(ctx, opts, "size");
    if (JS_IsObject(sizeVal)) {
        node->setSize(
            (float)qjsbind::get_prop_number(ctx, sizeVal, "start", 0.1),
            (float)qjsbind::get_prop_number(ctx, sizeVal, "end", 0));
    } else if (JS_IsNumber(sizeVal)) {
        float v = (float)jsNum(ctx, sizeVal);
        node->setSize(v, v);
    }
    JS_FreeValue(ctx, sizeVal);

    // color: { start, end } | "css" | gradient array of "css" strings /
    // {t, color} stops (unspecified t spreads the stops evenly over life)
    JSValue colorVal = JS_GetPropertyStr(ctx, opts, "color");
    if (JS_IsArray(colorVal)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, colorVal, "length");
        int32_t len = 0; JS_ToInt32(ctx, &len, lenVal); JS_FreeValue(ctx, lenVal);
        std::vector<std::pair<float, bromath::Color>> stops;
        for (int32_t i = 0; i < len; ++i) {
            JSValue e = JS_GetPropertyUint32(ctx, colorVal, (uint32_t)i);
            float t = (len > 1) ? (float)i / (float)(len - 1) : 0.0f;
            bromath::Color c = bromath::cfromColor8({255, 255, 255, 255});
            if (JS_IsString(e)) {
                c = colorFromJS(ctx, e, c);
            } else if (JS_IsObject(e)) {
                t = (float)qjsbind::get_prop_number(ctx, e, "t", t);
                JSValue cv = JS_GetPropertyStr(ctx, e, "color");
                c = colorFromJS(ctx, cv, c);
                JS_FreeValue(ctx, cv);
            }
            stops.emplace_back(t, c);
            JS_FreeValue(ctx, e);
        }
        std::stable_sort(stops.begin(), stops.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        node->setColorStops(std::move(stops));
    } else if (JS_IsObject(colorVal)) {
        JSValue cs = JS_GetPropertyStr(ctx, colorVal, "start");
        JSValue ce = JS_GetPropertyStr(ctx, colorVal, "end");
        bromath::Color start = colorFromJS(ctx, cs, bromath::cfromColor8({255,255,255,255}));
        bromath::Color end   = colorFromJS(ctx, ce, bromath::Color{start.r, start.g, start.b, 0.0f});
        node->setColors(start, end);
        JS_FreeValue(ctx, cs); JS_FreeValue(ctx, ce);
    } else if (JS_IsString(colorVal)) {
        bromath::Color c = colorFromJS(ctx, colorVal, bromath::cfromColor8({255,255,255,255}));
        bromath::Color end = c; end.a = 0.0f;
        node->setColors(c, end);
    }
    JS_FreeValue(ctx, colorVal);

    // rotation: { start, spinSpeed, spinSpread } (degrees)
    JSValue rotVal = JS_GetPropertyStr(ctx, opts, "rotation");
    if (JS_IsObject(rotVal)) {
        node->setRotation(
            (float)qjsbind::get_prop_number(ctx, rotVal, "start", 0),
            (float)qjsbind::get_prop_number(ctx, rotVal, "spinSpeed", 0),
            (float)qjsbind::get_prop_number(ctx, rotVal, "spinSpread", 0));
    }
    JS_FreeValue(ctx, rotVal);

    // drag (per-second velocity multiplier)
    JSValue dragVal = JS_GetPropertyStr(ctx, opts, "drag");
    if (JS_IsNumber(dragVal)) node->setDrag((float)jsNum(ctx, dragVal));
    JS_FreeValue(ctx, dragVal);

    // softness (world-units depth fade at geometry intersections; 0 = off)
    JSValue softVal = JS_GetPropertyStr(ctx, opts, "softness");
    if (JS_IsNumber(softVal)) node->setSoftness((float)jsNum(ctx, softVal));
    JS_FreeValue(ctx, softVal);

    // duration / loop: duration>0 without loop = one-shot (fires onFinished)
    JSValue durVal = JS_GetPropertyStr(ctx, opts, "duration");
    JSValue loopVal = JS_GetPropertyStr(ctx, opts, "loop");
    if (JS_IsNumber(durVal)) {
        float dur = (float)jsNum(ctx, durVal);
        bool loop = JS_IsUndefined(loopVal) ? false : JS_ToBool(ctx, loopVal);
        node->setDuration(dur, loop);
    } else if (!JS_IsUndefined(loopVal)) {
        node->setDuration(node->duration(), JS_ToBool(ctx, loopVal));
    }
    JS_FreeValue(ctx, durVal);
    JS_FreeValue(ctx, loopVal);

    // onFinished
    JSValue finVal = JS_GetPropertyStr(ctx, opts, "onFinished");
    if (!JS_IsUndefined(finVal)) installParticles3DOnFinished(ctx, finVal, node);
    JS_FreeValue(ctx, finVal);

    // initial burst
    JSValue burstVal = JS_GetPropertyStr(ctx, opts, "burst");
    if (JS_IsNumber(burstVal)) {
        int32_t n = 0; JS_ToInt32(ctx, &n, burstVal);
        node->burst(n);
    }
    JS_FreeValue(ctx, burstVal);

    // autoplay (default true)
    JSValue apVal = JS_GetPropertyStr(ctx, opts, "autoplay");
    bool autoplay = JS_IsUndefined(apVal) ? true : JS_ToBool(ctx, apVal);
    JS_FreeValue(ctx, apVal);
    if (!autoplay) node->stop();
}

// createParticles3D(opts?) → SceneNode (Particles3DNode)
JSValue js_sg_createParticles3D(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;
    auto* node = g->createParticles3D();
    g->root()->addChild(node);
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];
        JSValue nameVal = JS_GetPropertyStr(ctx, opts, "name");
        if (JS_IsString(nameVal)) node->setName(jsStr(ctx, nameVal));
        JS_FreeValue(ctx, nameVal);
        bromath::Vec3 pos;
        if (parseVec3Prop(ctx, opts, "position", pos)) node->setPosition(pos);
        applyParticle3DOpts(ctx, opts, node);
    }
    return wrapNode(ctx, node, g);
}

// createParticles(opts?) → SceneNode (ParticleNode)
JSValue js_sg_createParticles(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;
    auto* node = g->createParticles();
    g->root()->addChild(node);
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];
        JSValue nameVal = JS_GetPropertyStr(ctx, opts, "name");
        if (JS_IsString(nameVal)) node->setName(jsStr(ctx, nameVal));
        JS_FreeValue(ctx, nameVal);
        JSValue xVal = JS_GetPropertyStr(ctx, opts, "x");
        JSValue yVal = JS_GetPropertyStr(ctx, opts, "y");
        if (!JS_IsUndefined(xVal) || !JS_IsUndefined(yVal))
            node->setPosition((float)jsNum(ctx, xVal), (float)jsNum(ctx, yVal));
        JS_FreeValue(ctx, xVal);
        JS_FreeValue(ctx, yVal);
        applyParticleOpts(ctx, opts, node);
    }
    return wrapNode(ctx, node, g);
}

JSValue js_particles_burst(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node()) return JS_UNDEFINED;
    int32_t n = 1;
    if (argc > 0) JS_ToInt32(ctx, &n, argv[0]);
    if (w->node()->type() == scene::SceneNode::Type::Particles)
        static_cast<scene::ParticleNode*>(w->node())->burst(n);
    else if (w->node()->type() == scene::SceneNode::Type::Particles3D)
        static_cast<scene::Particles3DNode*>(w->node())->burst(n);
    return JS_DupValue(ctx, this_val);
}

JSValue js_particles_clear(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Particles)
        static_cast<scene::ParticleNode*>(w->node())->clear();
    else if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Particles3D)
        static_cast<scene::Particles3DNode*>(w->node())->clear();
    return JS_DupValue(ctx, this_val);
}

JSValue js_particles_configure(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node() || argc < 1 || !JS_IsObject(argv[0])) return JS_DupValue(ctx, this_val);
    if (w->node()->type() == scene::SceneNode::Type::Particles)
        applyParticleOpts(ctx, argv[0], static_cast<scene::ParticleNode*>(w->node()));
    else if (w->node()->type() == scene::SceneNode::Type::Particles3D)
        applyParticle3DOpts(ctx, argv[0], static_cast<scene::Particles3DNode*>(w->node()));
    return JS_DupValue(ctx, this_val);
}

// createShape(opts?) → SceneNode (ShapeNode)
JSValue js_sg_createShape(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;

    auto* node = g->createShape();
    g->root()->addChild(node);

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];

        // Shape type
        std::string shapeStr = qjsbind::get_prop_string(ctx, opts, "shape", "rect");
        if (shapeStr == "rect")        node->setShape(scene::ShapeNode::Shape::Rect);
        else if (shapeStr == "roundrect") node->setShape(scene::ShapeNode::Shape::RoundRect);
        else if (shapeStr == "circle")  node->setShape(scene::ShapeNode::Shape::Circle);
        else if (shapeStr == "ellipse") node->setShape(scene::ShapeNode::Shape::Ellipse);
        else if (shapeStr == "polygon") node->setShape(scene::ShapeNode::Shape::Polygon);
        else if (shapeStr == "line")    node->setShape(scene::ShapeNode::Shape::Line);

        // Name
        JSValue nameVal = JS_GetPropertyStr(ctx, opts, "name");
        if (JS_IsString(nameVal)) node->setName(jsStr(ctx, nameVal));
        JS_FreeValue(ctx, nameVal);

        // Dimensions
        JSValue wVal = JS_GetPropertyStr(ctx, opts, "width");
        JSValue hVal = JS_GetPropertyStr(ctx, opts, "height");
        if (!JS_IsUndefined(wVal) || !JS_IsUndefined(hVal))
            node->setSize(jsNum(ctx, wVal), jsNum(ctx, hVal));
        JS_FreeValue(ctx, wVal);
        JS_FreeValue(ctx, hVal);

        // Radius
        JSValue rVal = JS_GetPropertyStr(ctx, opts, "radius");
        if (!JS_IsUndefined(rVal)) node->setRadius(jsNum(ctx, rVal));
        JS_FreeValue(ctx, rVal);

        // Corner radius
        JSValue crVal = JS_GetPropertyStr(ctx, opts, "cornerRadius");
        if (!JS_IsUndefined(crVal)) node->setCornerRadius(jsNum(ctx, crVal));
        JS_FreeValue(ctx, crVal);

        // Radii (ellipse)
        JSValue rxVal = JS_GetPropertyStr(ctx, opts, "radiusX");
        JSValue ryVal = JS_GetPropertyStr(ctx, opts, "radiusY");
        if (!JS_IsUndefined(rxVal) || !JS_IsUndefined(ryVal))
            node->setRadii(jsNum(ctx, rxVal), jsNum(ctx, ryVal));
        JS_FreeValue(ctx, rxVal);
        JS_FreeValue(ctx, ryVal);

        // Fill color
        JSValue fillVal = JS_GetPropertyStr(ctx, opts, "fill");
        if (!JS_IsUndefined(fillVal)) {
            uint8_t r, g, b, a;
            if (JS_IsString(fillVal) && parseColor(jsStr(ctx, fillVal), r, g, b, a)) {
                node->setFillColor(bromath::cfromColor8({r, g, b, a}));
            }
        } else {
            node->setHasFill(true); // default white fill
        }
        JS_FreeValue(ctx, fillVal);

        // Stroke color
        JSValue strokeVal = JS_GetPropertyStr(ctx, opts, "stroke");
        if (!JS_IsUndefined(strokeVal)) {
            uint8_t r, g, b, a;
            if (JS_IsString(strokeVal) && parseColor(jsStr(ctx, strokeVal), r, g, b, a)) {
                node->setStrokeColor(bromath::cfromColor8({r, g, b, a}));
            }
        }
        JS_FreeValue(ctx, strokeVal);

        // Stroke width
        JSValue swVal = JS_GetPropertyStr(ctx, opts, "strokeWidth");
        if (!JS_IsUndefined(swVal)) {
            node->setStrokeWidth(jsNum(ctx, swVal));
            node->setHasStroke(true);
        }
        JS_FreeValue(ctx, swVal);

        // Anchor
        JSValue axVal = JS_GetPropertyStr(ctx, opts, "anchorX");
        JSValue ayVal = JS_GetPropertyStr(ctx, opts, "anchorY");
        if (!JS_IsUndefined(axVal) || !JS_IsUndefined(ayVal))
            node->setAnchor(
                JS_IsUndefined(axVal) ? 0.5f : (float)jsNum(ctx, axVal),
                JS_IsUndefined(ayVal) ? 0.5f : (float)jsNum(ctx, ayVal));
        JS_FreeValue(ctx, axVal);
        JS_FreeValue(ctx, ayVal);

        // Position
        JSValue xVal = JS_GetPropertyStr(ctx, opts, "x");
        JSValue yVal = JS_GetPropertyStr(ctx, opts, "y");
        if (!JS_IsUndefined(xVal) || !JS_IsUndefined(yVal))
            node->setPosition(
                JS_IsUndefined(xVal) ? 0.0f : (float)jsNum(ctx, xVal),
                JS_IsUndefined(yVal) ? 0.0f : (float)jsNum(ctx, yVal));
        JS_FreeValue(ctx, xVal);
        JS_FreeValue(ctx, yVal);

        // Points (polygon)
        JSValue ptsVal = JS_GetPropertyStr(ctx, opts, "points");
        if (JS_IsArray(ptsVal)) {
            JSValue lenVal = JS_GetPropertyStr(ctx, ptsVal, "length");
            int32_t len = 0;
            JS_ToInt32(ctx, &len, lenVal);
            JS_FreeValue(ctx, lenVal);
            std::vector<float> pts(len);
            for (int32_t i = 0; i < len; i++) {
                JSValue elem = JS_GetPropertyUint32(ctx, ptsVal, i);
                pts[i] = (float)jsNum(ctx, elem);
                JS_FreeValue(ctx, elem);
            }
            node->setPoints(pts);
        }
        JS_FreeValue(ctx, ptsVal);

        applyBillboardOpts(ctx, opts, node);
    }

    return wrapNode(ctx, node, g);
}

// Parse a `sheet` option onto a SpriteNode. Two forms:
//   { frameWidth, frameHeight, columns, rows }     -- uniform grid
//   { frames: [{x,y,w,h}, ...] }                  -- explicit list
static void applySpriteSheet(JSContext* ctx, JSValueConst opts, scene::SpriteNode* node) {
    JSValue sheetVal = JS_GetPropertyStr(ctx, opts, "sheet");
    if (!JS_IsObject(sheetVal)) { JS_FreeValue(ctx, sheetVal); return; }

    JSValue framesVal = JS_GetPropertyStr(ctx, sheetVal, "frames");
    if (JS_IsArray(framesVal)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, framesVal, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        std::vector<scene::SpriteNode::Frame> frames;
        frames.reserve(len);
        for (int32_t i = 0; i < len; ++i) {
            JSValue f = JS_GetPropertyUint32(ctx, framesVal, i);
            scene::SpriteNode::Frame fr{};
            fr.x = (float)qjsbind::get_prop_number(ctx, f, "x", 0);
            fr.y = (float)qjsbind::get_prop_number(ctx, f, "y", 0);
            fr.w = (float)qjsbind::get_prop_number(ctx, f, "w", 0);
            fr.h = (float)qjsbind::get_prop_number(ctx, f, "h", 0);
            frames.push_back(fr);
            JS_FreeValue(ctx, f);
        }
        node->setSheetFrames(std::move(frames));
    } else {
        int fw = (int)qjsbind::get_prop_number(ctx, sheetVal, "frameWidth", 0);
        int fh = (int)qjsbind::get_prop_number(ctx, sheetVal, "frameHeight", 0);
        int cols = (int)qjsbind::get_prop_number(ctx, sheetVal, "columns", 0);
        int rows = (int)qjsbind::get_prop_number(ctx, sheetVal, "rows", 0);
        node->setSheetGrid(fw, fh, cols, rows);
    }
    JS_FreeValue(ctx, framesVal);
    JS_FreeValue(ctx, sheetVal);
}

// Parse an animation spec object: { frames: [...], fps, loop, next }.
static scene::SpriteNode::AnimationSpec parseAnimSpec(JSContext* ctx, JSValueConst obj) {
    scene::SpriteNode::AnimationSpec spec;
    JSValue framesVal = JS_GetPropertyStr(ctx, obj, "frames");
    if (JS_IsArray(framesVal)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, framesVal, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        spec.frames.reserve(len);
        for (int32_t i = 0; i < len; ++i) {
            JSValue elem = JS_GetPropertyUint32(ctx, framesVal, i);
            int32_t v = 0;
            JS_ToInt32(ctx, &v, elem);
            spec.frames.push_back(v);
            JS_FreeValue(ctx, elem);
        }
    }
    JS_FreeValue(ctx, framesVal);

    JSValue fpsVal = JS_GetPropertyStr(ctx, obj, "fps");
    if (!JS_IsUndefined(fpsVal)) spec.fps = (float)jsNum(ctx, fpsVal);
    JS_FreeValue(ctx, fpsVal);

    JSValue loopVal = JS_GetPropertyStr(ctx, obj, "loop");
    if (!JS_IsUndefined(loopVal)) spec.loop = JS_ToBool(ctx, loopVal);
    JS_FreeValue(ctx, loopVal);

    JSValue nextVal = JS_GetPropertyStr(ctx, obj, "next");
    if (JS_IsString(nextVal)) spec.next = jsStr(ctx, nextVal);
    JS_FreeValue(ctx, nextVal);

    return spec;
}

// Parse `animations: { name: spec, ... }` into the SpriteNode.
static void applySpriteAnimations(JSContext* ctx, JSValueConst opts, scene::SpriteNode* node) {
    JSValue animsVal = JS_GetPropertyStr(ctx, opts, "animations");
    if (!JS_IsObject(animsVal)) { JS_FreeValue(ctx, animsVal); return; }

    JSPropertyEnum* tab = nullptr;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &count, animsVal,
                                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
        for (uint32_t i = 0; i < count; ++i) {
            const char* keyStr = JS_AtomToCString(ctx, tab[i].atom);
            if (!keyStr) continue;
            JSValue v = JS_GetProperty(ctx, animsVal, tab[i].atom);
            if (JS_IsObject(v)) {
                node->addAnimation(keyStr, parseAnimSpec(ctx, v));
            }
            JS_FreeValue(ctx, v);
            JS_FreeCString(ctx, keyStr);
        }
        for (uint32_t i = 0; i < count; ++i) JS_FreeAtom(ctx, tab[i].atom);
        js_free(ctx, tab);
    }
    JS_FreeValue(ctx, animsVal);
}

// createSprite(opts?) → SceneNode (SpriteNode)
JSValue js_sg_createSprite(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;

    auto* node = g->createSprite();
    g->root()->addChild(node);

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];

        JSValue nameVal = JS_GetPropertyStr(ctx, opts, "name");
        if (JS_IsString(nameVal)) node->setName(jsStr(ctx, nameVal));
        JS_FreeValue(ctx, nameVal);

        // Image path
        JSValue srcVal = JS_GetPropertyStr(ctx, opts, "src");
        if (JS_IsString(srcVal)) node->setImagePath(jsStr(ctx, srcVal));
        JS_FreeValue(ctx, srcVal);

        // Size
        JSValue wVal = JS_GetPropertyStr(ctx, opts, "width");
        JSValue hVal = JS_GetPropertyStr(ctx, opts, "height");
        if (!JS_IsUndefined(wVal) || !JS_IsUndefined(hVal))
            node->setSize((float)jsNum(ctx, wVal), (float)jsNum(ctx, hVal));
        JS_FreeValue(ctx, wVal);
        JS_FreeValue(ctx, hVal);

        // Position
        JSValue xVal = JS_GetPropertyStr(ctx, opts, "x");
        JSValue yVal = JS_GetPropertyStr(ctx, opts, "y");
        if (!JS_IsUndefined(xVal) || !JS_IsUndefined(yVal))
            node->setPosition((float)jsNum(ctx, xVal), (float)jsNum(ctx, yVal));
        JS_FreeValue(ctx, xVal);
        JS_FreeValue(ctx, yVal);

        // Opacity
        JSValue oVal = JS_GetPropertyStr(ctx, opts, "opacity");
        if (!JS_IsUndefined(oVal)) node->setOpacity((float)jsNum(ctx, oVal));
        JS_FreeValue(ctx, oVal);

        // Anchor
        JSValue axVal = JS_GetPropertyStr(ctx, opts, "anchorX");
        JSValue ayVal = JS_GetPropertyStr(ctx, opts, "anchorY");
        if (!JS_IsUndefined(axVal) || !JS_IsUndefined(ayVal))
            node->setAnchor(
                JS_IsUndefined(axVal) ? 0.5f : (float)jsNum(ctx, axVal),
                JS_IsUndefined(ayVal) ? 0.5f : (float)jsNum(ctx, ayVal));
        JS_FreeValue(ctx, axVal);
        JS_FreeValue(ctx, ayVal);

        // Spritesheet config + named animations + initial play.
        applySpriteSheet(ctx, opts, node);
        applySpriteAnimations(ctx, opts, node);
        JSValue playVal = JS_GetPropertyStr(ctx, opts, "play");
        if (JS_IsString(playVal)) node->play(jsStr(ctx, playVal));
        JS_FreeValue(ctx, playVal);
        JSValue fiVal = JS_GetPropertyStr(ctx, opts, "frameIndex");
        if (JS_IsNumber(fiVal)) {
            int32_t idx = 0; JS_ToInt32(ctx, &idx, fiVal);
            node->setFrameIndex(idx);
        }
        JS_FreeValue(ctx, fiVal);

        applyBillboardOpts(ctx, opts, node);
    }

    return wrapNode(ctx, node, g);
}

// createHtmlNode({html, width, height, pxPerUnit, worldAnchor, billboard, name?})
JSValue js_sg_createHtml(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;

    auto* node = g->createHtml();
    g->root()->addChild(node);

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];

        JSValue nameVal = JS_GetPropertyStr(ctx, opts, "name");
        if (JS_IsString(nameVal)) node->setName(jsStr(ctx, nameVal));
        JS_FreeValue(ctx, nameVal);

        float w = 200.0f, h = 50.0f;
        JSValue wVal = JS_GetPropertyStr(ctx, opts, "width");
        JSValue hVal = JS_GetPropertyStr(ctx, opts, "height");
        if (!JS_IsUndefined(wVal)) w = (float)jsNum(ctx, wVal);
        if (!JS_IsUndefined(hVal)) h = (float)jsNum(ctx, hVal);
        JS_FreeValue(ctx, wVal);
        JS_FreeValue(ctx, hVal);
        node->setLayoutSize(w, h);

        JSValue ppuVal = JS_GetPropertyStr(ctx, opts, "pxPerUnit");
        if (!JS_IsUndefined(ppuVal)) node->setPxPerUnit((float)jsNum(ctx, ppuVal));
        JS_FreeValue(ctx, ppuVal);

        JSValue htmlVal = JS_GetPropertyStr(ctx, opts, "html");
        if (JS_IsString(htmlVal)) node->setHtml(jsStr(ctx, htmlVal));
        JS_FreeValue(ctx, htmlVal);

        applyBillboardOpts(ctx, opts, node);
    }

    return wrapNode(ctx, node, g);
}

// createGaussianSplat({path, x, y, z, scale, name})
// Loads a 3D Gaussian Splat .ply and renders it with EWA splatting via
// GaussianSplatNode. `path` is resolved against the app base dir + mounts.
JSValue js_sg_createGaussianSplat(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;

    auto* node = g->createGaussianSplat();
    g->root()->addChild(node);

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];

        JSValue nameVal = JS_GetPropertyStr(ctx, opts, "name");
        if (JS_IsString(nameVal)) node->setName(jsStr(ctx, nameVal));
        JS_FreeValue(ctx, nameVal);

        JSValue pathVal = JS_GetPropertyStr(ctx, opts, "path");
        if (JS_IsString(pathVal)) {
            std::string path = resolveAppPath(jsStr(ctx, pathVal));
            auto cloud = bromesh::loadSplatPLY(path);
            if (cloud.empty()) {
                LOG_WARN("createGaussianSplat: failed to load splat ply '%s'", path.c_str());
            } else {
                auto b = cloud.bounds();
                LOG_INFO("createGaussianSplat: loaded %zu splats (SH deg %d) from '%s'\n"
                         "  bounds min=(%.3f %.3f %.3f) max=(%.3f %.3f %.3f) center=(%.3f %.3f %.3f)",
                         cloud.count(), cloud.shDegree, path.c_str(),
                         b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z,
                         (b.min.x + b.max.x) * 0.5f, (b.min.y + b.max.y) * 0.5f,
                         (b.min.z + b.max.z) * 0.5f);
                node->setCloud(std::move(cloud));
            }
        }
        JS_FreeValue(ctx, pathVal);

        // In-memory cloud — the SoA typed arrays bro.triposplat.generate returns
        // ({ positions, scales, rotations, opacities, sh, shDegree }). Field
        // layout already matches bromesh::GaussianSplatCloud, so this is a copy.
        JSValue cloudVal = JS_GetPropertyStr(ctx, opts, "cloud");
        if (JS_IsObject(cloudVal)) {
            auto readF32 = [&](const char* key, std::vector<float>& dst) {
                JSValue v = JS_GetPropertyStr(ctx, cloudVal, key);
                std::size_t off = 0, len = 0, bpe = 0;
                JSValue ab = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
                if (!JS_IsException(ab)) {
                    std::size_t blen = 0;
                    std::uint8_t* p = JS_GetArrayBuffer(ctx, &blen, ab);
                    if (p && len > 0) {
                        const float* f = reinterpret_cast<const float*>(p + off);
                        dst.assign(f, f + len / sizeof(float));
                    }
                } else {
                    JS_FreeValue(ctx, JS_GetException(ctx));
                }
                JS_FreeValue(ctx, ab);
                JS_FreeValue(ctx, v);
            };
            bromesh::GaussianSplatCloud cloud;
            readF32("positions", cloud.positions);
            readF32("scales",    cloud.scales);
            readF32("rotations", cloud.rotations);
            readF32("opacities", cloud.opacities);
            readF32("sh",        cloud.sh);
            cloud.shDegree = (int)qjsbind::get_prop_number(ctx, cloudVal, "shDegree", 0);
            if (cloud.empty()) {
                LOG_WARN("createGaussianSplat: opts.cloud has no positions");
            } else {
                LOG_INFO("createGaussianSplat: %zu splats from in-memory cloud (SH deg %d)",
                         cloud.count(), cloud.shDegree);
                node->setCloud(std::move(cloud));
            }
        }
        JS_FreeValue(ctx, cloudVal);

        double x = qjsbind::get_prop_number(ctx, opts, "x", 0);
        double y = qjsbind::get_prop_number(ctx, opts, "y", 0);
        double z = qjsbind::get_prop_number(ctx, opts, "z", 0);
        node->setPosition((float)x, (float)y, (float)z);

        JSValue scaleVal = JS_GetPropertyStr(ctx, opts, "scale");
        if (!JS_IsUndefined(scaleVal)) {
            double s = 1;
            JS_ToFloat64(ctx, &s, scaleVal);
            node->setScale((float)s, (float)s, (float)s);
        }
        JS_FreeValue(ctx, scaleVal);
    }

    return wrapNode(ctx, node, g);
}

// createDecal({ name, x, y, z, size|scale, rx, ry, rz, texture,
//               emissionTexture, modulate, emissionStrength, upperFade,
//               lowerFade, normalFade, renderPriority }) → DecalNode
// Projected decal (Godot Decal analog). The decal volume is the unit box
// scaled by the node's scale — `size` is an alias for `scale` (uniform
// number or [x, y, z]); projection runs along local -Y. Textures use the
// same { width, height, data: Uint8Array(rgba8) } shape as createMesh.
// See docs/scene-api.js for the full contract + limitations vs Godot.
JSValue js_sg_createDecal(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;

    auto* node = g->createDecal();
    g->root()->addChild(node);

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];

        JSValue nameVal = JS_GetPropertyStr(ctx, opts, "name");
        if (JS_IsString(nameVal)) node->setName(jsStr(ctx, nameVal));
        JS_FreeValue(ctx, nameVal);

        double x = qjsbind::get_prop_number(ctx, opts, "x", 0);
        double y = qjsbind::get_prop_number(ctx, opts, "y", 0);
        double z = qjsbind::get_prop_number(ctx, opts, "z", 0);
        node->setPosition((float)x, (float)y, (float)z);

        // Box size = node scale. `size` and `scale` are interchangeable
        // (size wins when both are given); uniform number or [x, y, z].
        auto applyScale = [&](const char* key) -> bool {
            JSValue v = JS_GetPropertyStr(ctx, opts, key);
            bool took = false;
            if (JS_IsArray(v)) {
                double s3[3] = {1, 1, 1};
                for (uint32_t i = 0; i < 3; ++i) {
                    JSValue e = JS_GetPropertyUint32(ctx, v, i);
                    if (!JS_IsUndefined(e)) JS_ToFloat64(ctx, &s3[i], e);
                    JS_FreeValue(ctx, e);
                }
                node->setScale((float)s3[0], (float)s3[1], (float)s3[2]);
                took = true;
            } else if (!JS_IsUndefined(v)) {
                double s = 1;
                JS_ToFloat64(ctx, &s, v);
                node->setScale((float)s, (float)s, (float)s);
                took = true;
            }
            JS_FreeValue(ctx, v);
            return took;
        };
        if (!applyScale("size")) applyScale("scale");

        // Rotation (Euler degrees, matching createMesh).
        JSValue rxVal = JS_GetPropertyStr(ctx, opts, "rx");
        JSValue ryVal = JS_GetPropertyStr(ctx, opts, "ry");
        JSValue rzVal = JS_GetPropertyStr(ctx, opts, "rz");
        if (!JS_IsUndefined(rxVal) || !JS_IsUndefined(ryVal) || !JS_IsUndefined(rzVal)) {
            double rx = 0, ry = 0, rz = 0;
            if (!JS_IsUndefined(rxVal)) JS_ToFloat64(ctx, &rx, rxVal);
            if (!JS_IsUndefined(ryVal)) JS_ToFloat64(ctx, &ry, ryVal);
            if (!JS_IsUndefined(rzVal)) JS_ToFloat64(ctx, &rz, rzVal);
            const float toRad = 3.14159265f / 180.0f;
            node->setRotationEuler((float)rx * toRad, (float)ry * toRad,
                                   (float)rz * toRad);
        }
        JS_FreeValue(ctx, rxVal);
        JS_FreeValue(ctx, ryVal);
        JS_FreeValue(ctx, rzVal);

        // Modulate — CSS color string or [r, g, b, a?] floats.
        JSValue modVal = JS_GetPropertyStr(ctx, opts, "modulate");
        if (JS_IsString(modVal)) {
            uint8_t r, gc, b, a;
            if (parseColor(jsStr(ctx, modVal), r, gc, b, a))
                node->setModulate(r / 255.0f, gc / 255.0f, b / 255.0f, a / 255.0f);
        } else if (JS_IsArray(modVal)) {
            double m4[4] = {1, 1, 1, 1};
            for (uint32_t i = 0; i < 4; ++i) {
                JSValue e = JS_GetPropertyUint32(ctx, modVal, i);
                if (!JS_IsUndefined(e)) JS_ToFloat64(ctx, &m4[i], e);
                JS_FreeValue(ctx, e);
            }
            node->setModulate((float)m4[0], (float)m4[1], (float)m4[2], (float)m4[3]);
        }
        JS_FreeValue(ctx, modVal);

        node->setEmissionStrength((float)qjsbind::get_prop_number(
            ctx, opts, "emissionStrength", 1.0));
        node->setUpperFade((float)qjsbind::get_prop_number(ctx, opts, "upperFade", 0.0));
        node->setLowerFade((float)qjsbind::get_prop_number(ctx, opts, "lowerFade", 0.0));
        node->setNormalFade((float)qjsbind::get_prop_number(ctx, opts, "normalFade", 0.0));
        node->setRenderPriority((int)qjsbind::get_prop_number(ctx, opts, "renderPriority", 0.0));

        // Textures — { width, height, data: Uint8Array(rgba8) }, the same
        // shape as createMesh's texture maps.
        auto applyTex = [&](const char* key,
                            void (scene::DecalNode::*setter)(int, int, const uint8_t*)) {
            JSValue tex = JS_GetPropertyStr(ctx, opts, key);
            if (JS_IsObject(tex)) {
                int w = (int)qjsbind::get_prop_number(ctx, tex, "width", 0);
                int h = (int)qjsbind::get_prop_number(ctx, tex, "height", 0);
                JSValue dataVal = JS_GetPropertyStr(ctx, tex, "data");
                size_t off = 0, len = 0;
                JSValue ab = JS_GetTypedArrayBuffer(ctx, dataVal, &off, &len, nullptr);
                if (!JS_IsException(ab)) {
                    size_t bytes = 0;
                    uint8_t* base = JS_GetArrayBuffer(ctx, &bytes, ab);
                    if (base && w > 0 && h > 0 && len >= (size_t)w * (size_t)h * 4) {
                        (node->*setter)(w, h, base + off);
                    }
                    JS_FreeValue(ctx, ab);
                }
                JS_FreeValue(ctx, dataVal);
            }
            JS_FreeValue(ctx, tex);
        };
        applyTex("texture",         &scene::DecalNode::setAlbedoTexture);
        applyTex("emissionTexture", &scene::DecalNode::setEmissionTexture);
    }

    return wrapNode(ctx, node, g);
}

// createReflectionProbe({ name, x, y, z, size|scale, rx, ry, rz, resolution,
//                         updateMode: 'once'|'manual', boxProjection,
//                         intensity, interior, priority }) → ReflectionProbeNode
// Local reflection probe (Godot ReflectionProbe analog). The probe volume is
// the unit box scaled by the node's scale — `size` is an alias for `scale`
// (uniform number or [x, y, z]), matching createDecal — and the capture
// origin is the node's world position. There is NO per-frame auto update
// mode: 'once' (default) captures on the first visible frame, 'manual' waits
// for probe.capture(). See docs/scene-api.js for the full contract.
JSValue js_sg_createReflectionProbe(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;

    auto* node = g->createReflectionProbe();
    g->root()->addChild(node);

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];

        JSValue nameVal = JS_GetPropertyStr(ctx, opts, "name");
        if (JS_IsString(nameVal)) node->setName(jsStr(ctx, nameVal));
        JS_FreeValue(ctx, nameVal);

        double x = qjsbind::get_prop_number(ctx, opts, "x", 0);
        double y = qjsbind::get_prop_number(ctx, opts, "y", 0);
        double z = qjsbind::get_prop_number(ctx, opts, "z", 0);
        node->setPosition((float)x, (float)y, (float)z);

        // Box size = node scale. `size` and `scale` interchangeable (size
        // wins when both are given); uniform number or [x, y, z].
        auto applyScale = [&](const char* key) -> bool {
            JSValue v = JS_GetPropertyStr(ctx, opts, key);
            bool took = false;
            if (JS_IsArray(v)) {
                double s3[3] = {1, 1, 1};
                for (uint32_t i = 0; i < 3; ++i) {
                    JSValue e = JS_GetPropertyUint32(ctx, v, i);
                    if (!JS_IsUndefined(e)) JS_ToFloat64(ctx, &s3[i], e);
                    JS_FreeValue(ctx, e);
                }
                node->setScale((float)s3[0], (float)s3[1], (float)s3[2]);
                took = true;
            } else if (!JS_IsUndefined(v)) {
                double s = 1;
                JS_ToFloat64(ctx, &s, v);
                node->setScale((float)s, (float)s, (float)s);
                took = true;
            }
            JS_FreeValue(ctx, v);
            return took;
        };
        if (!applyScale("size")) applyScale("scale");

        // Rotation (Euler degrees, matching createMesh / createDecal).
        JSValue rxVal = JS_GetPropertyStr(ctx, opts, "rx");
        JSValue ryVal = JS_GetPropertyStr(ctx, opts, "ry");
        JSValue rzVal = JS_GetPropertyStr(ctx, opts, "rz");
        if (!JS_IsUndefined(rxVal) || !JS_IsUndefined(ryVal) || !JS_IsUndefined(rzVal)) {
            double rx = 0, ry = 0, rz = 0;
            if (!JS_IsUndefined(rxVal)) JS_ToFloat64(ctx, &rx, rxVal);
            if (!JS_IsUndefined(ryVal)) JS_ToFloat64(ctx, &ry, ryVal);
            if (!JS_IsUndefined(rzVal)) JS_ToFloat64(ctx, &rz, rzVal);
            const float toRad = 3.14159265f / 180.0f;
            node->setRotationEuler((float)rx * toRad, (float)ry * toRad,
                                   (float)rz * toRad);
        }
        JS_FreeValue(ctx, rxVal);
        JS_FreeValue(ctx, ryVal);
        JS_FreeValue(ctx, rzVal);

        node->setResolution((int)qjsbind::get_prop_number(
            ctx, opts, "resolution", 128.0));
        node->setIntensity((float)qjsbind::get_prop_number(
            ctx, opts, "intensity", 1.0));
        node->setInterior((float)qjsbind::get_prop_number(
            ctx, opts, "interior", 0.0));
        node->setPriority((int)qjsbind::get_prop_number(
            ctx, opts, "priority", 0.0));

        JSValue bpVal = JS_GetPropertyStr(ctx, opts, "boxProjection");
        if (!JS_IsUndefined(bpVal))
            node->setBoxProjection(JS_ToBool(ctx, bpVal) != 0);
        JS_FreeValue(ctx, bpVal);

        JSValue umVal = JS_GetPropertyStr(ctx, opts, "updateMode");
        if (JS_IsString(umVal)) {
            std::string m = jsStr(ctx, umVal);
            node->setUpdateMode(m == "manual"
                ? scene::ReflectionProbeNode::UpdateMode::Manual
                : scene::ReflectionProbeNode::UpdateMode::Once);
        }
        JS_FreeValue(ctx, umVal);
    }

    return wrapNode(ctx, node, g);
}

// probe.capture() — request a (re)capture on the next rendered frame.
// ReflectionProbeNode only; a clean no-op-with-error elsewhere.
JSValue js_node_probeCapture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)argc; (void)argv;
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node() ||
        w->node()->type() != scene::SceneNode::Type::ReflectionProbe)
        return JS_ThrowTypeError(ctx, "capture: not a ReflectionProbeNode");
    static_cast<scene::ReflectionProbeNode*>(w->node())->requestCapture();
    return JS_UNDEFINED;
}

static inline void _unused_scene_fx_install(JSContext* ctx)
{
    // No install needed
    (void)ctx;
}

} // namespace bro::js

#endif // BRO_WITH_3D
