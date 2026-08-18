#pragma once

#include "js/flora_bindings.h"
#if BRO_WITH_FLORA

#include <broflora/broflora.h>
#include <bromath/vec.h>
#include <bromesh/mesh_data.h>
#include <bromesh/procedural/leaf_scatter.h>
#include <qjsbind/qjsbind.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bro::js {

// ── Opaque wrapper ─────────────────────────────────────────────────────
// The world owns prototypes, voronoi sites, and plants. Module instances
// inside plants reference prototypes by pointer; the wrapper guarantees
// the WorldState lives as long as the JS handle does so those pointers
// stay valid.
struct FloraWorldWrapper {
    std::unique_ptr<broflora::WorldState> world;
};
using FWW = FloraWorldWrapper;

// ── Vec3 / Float32Array helpers ────────────────────────────────────────

inline bool readVec3Prop(JSContext* ctx, JSValueConst obj, const char* prop,
                         bromath::Vec3& out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    if (!JS_IsArray(v)) { JS_FreeValue(ctx, v); return false; }
    for (int i = 0; i < 3; ++i) {
        JSValue el = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
        double d = 0;
        JS_ToFloat64(ctx, &d, el);
        JS_FreeValue(ctx, el);
        (&out.x)[i] = (float)d;
    }
    JS_FreeValue(ctx, v);
    return true;
}

inline JSValue makeVec3(JSContext* ctx, const bromath::Vec3& v) {
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, v.x));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, v.y));
    JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, v.z));
    return arr;
}

inline JSValue makeFloat32Array(JSContext* ctx, const float* data, size_t count) {
    size_t size = count * sizeof(float);
    JSValue abuf = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(data), size);
    if (JS_IsException(abuf)) return abuf;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "Float32Array");
    JS_FreeValue(ctx, global);
    JSValue arr = JS_CallConstructor(ctx, ctor, 1, &abuf);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, abuf);
    return arr;
}

inline void fillFoliageDensity(const std::vector<broflora::FoliageSample>& samples,
                               size_t segCount,
                               bromesh::LeafPlacementOptions& opts) {
    if (!opts.densityWeight.empty() || samples.size() != segCount) return;
    opts.densityWeight.resize(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto& f = samples[i];
        float exposure = 0.12f + 0.88f * f.lightExposure01;
        float maturity = std::min(1.0f, f.age01);
        float alive    = 1.0f - f.senescence01;
        float stem     = f.twigGrade01 * f.twigGrade01;        // sharpen the thin-shoot bias
        if (!f.isTerminal) stem *= 0.10f;                      // strongly suppress structural scaffold limbs
        opts.densityWeight[i] = exposure * maturity * alive * stem;
    }
}

// ── Optional-field readers ─────────────────────────────────────────────

inline bool readFloatField(JSContext* ctx, JSValueConst obj, const char* prop, float& out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return false; }
    double d = 0;
    if (JS_ToFloat64(ctx, &d, v) == 0) out = (float)d;
    JS_FreeValue(ctx, v);
    return true;
}

inline bool readUint32Field(JSContext* ctx, JSValueConst obj, const char* prop, uint32_t& out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return false; }
    uint32_t u = 0;
    if (JS_ToUint32(ctx, &u, v) == 0) out = u;
    JS_FreeValue(ctx, v);
    return true;
}

inline bool readIntField(JSContext* ctx, JSValueConst obj, const char* prop, int& out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return false; }
    int32_t i = 0;
    if (JS_ToInt32(ctx, &i, v) == 0) out = i;
    JS_FreeValue(ctx, v);
    return true;
}

inline bool readBoolField(JSContext* ctx, JSValueConst obj, const char* prop, bool& out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return false; }
    out = JS_ToBool(ctx, v) != 0;
    JS_FreeValue(ctx, v);
    return true;
}

inline bromesh::LeafShape parseLeafShapeValue(JSContext* ctx, JSValueConst v) {
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        bromesh::LeafShape r = bromesh::LeafShape::Oval;
        if (s) {
            if      (!std::strcmp(s, "oval"))    r = bromesh::LeafShape::Oval;
            else if (!std::strcmp(s, "pointed")) r = bromesh::LeafShape::Pointed;
            else if (!std::strcmp(s, "lobed"))   r = bromesh::LeafShape::Lobed;
            else if (!std::strcmp(s, "needle"))  r = bromesh::LeafShape::Needle;
            else if (!std::strcmp(s, "frond"))   r = bromesh::LeafShape::Frond;
            else if (!std::strcmp(s, "petal"))   r = bromesh::LeafShape::Petal;
            JS_FreeCString(ctx, s);
        }
        return r;
    } else if (JS_IsNumber(v)) {
        int32_t val = 0;
        JS_ToInt32(ctx, &val, v);
        if (val >= 0 && val <= 5) return static_cast<bromesh::LeafShape>(val);
    }
    return bromesh::LeafShape::Oval;
}

inline broflora::Phyllotaxy parsePhyllotaxy(JSContext* ctx, JSValueConst v) {
    if (JS_IsNumber(v)) {
        int32_t val = 0;
        JS_ToInt32(ctx, &val, v);
        if (val >= 0 && val <= 4) return static_cast<broflora::Phyllotaxy>(val);
    } else if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        broflora::Phyllotaxy p = broflora::Phyllotaxy::Alternate;
        if (s) {
            if (!std::strcmp(s, "alternate") || !std::strcmp(s, "Alternate")) p = broflora::Phyllotaxy::Alternate;
            else if (!std::strcmp(s, "opposite") || !std::strcmp(s, "Opposite")) p = broflora::Phyllotaxy::Opposite;
            else if (!std::strcmp(s, "spiral") || !std::strcmp(s, "Spiral")) p = broflora::Phyllotaxy::Spiral;
            else if (!std::strcmp(s, "fascicle") || !std::strcmp(s, "Fascicle")) p = broflora::Phyllotaxy::Fascicle;
            else if (!std::strcmp(s, "compoundPinnate") || !std::strcmp(s, "CompoundPinnate") ||
                     !std::strcmp(s, "compound_pinnate") || !std::strcmp(s, "pinnate")) p = broflora::Phyllotaxy::CompoundPinnate;
            JS_FreeCString(ctx, s);
        }
        return p;
    }
    return broflora::Phyllotaxy::Alternate;
}

inline void readLeafClusterOptions(JSContext* ctx, JSValueConst obj, broflora::LeafClusterOptions& opts) {
    if (!JS_IsObject(obj)) return;
    readIntField  (ctx, obj, "count",            opts.count);
    readFloatField(ctx, obj, "twigLength",       opts.twigLength);
    readFloatField(ctx, obj, "twigRadius",       opts.twigRadius);
    readFloatField(ctx, obj, "petioleLength",    opts.petioleLength);
    readFloatField(ctx, obj, "leafWidth",        opts.leafWidth);
    readFloatField(ctx, obj, "leafLength",       opts.leafLength);

    JSValue sv = JS_GetPropertyStr(ctx, obj, "leafShape");
    if (JS_IsUndefined(sv) || JS_IsNull(sv)) {
        JS_FreeValue(ctx, sv);
        sv = JS_GetPropertyStr(ctx, obj, "shape");
    }
    if (!JS_IsUndefined(sv) && !JS_IsNull(sv)) {
        opts.leafShape = parseLeafShapeValue(ctx, sv);
        opts.shape = opts.leafShape;
    }
    JS_FreeValue(ctx, sv);

    readFloatField(ctx, obj, "leafBend",         opts.leafBend);
    readFloatField(ctx, obj, "leafCurl",         opts.leafCurl);
    readFloatField(ctx, obj, "leafCup",          opts.leafCup);
    readFloatField(ctx, obj, "droop",            opts.droop);
    readFloatField(ctx, obj, "upBias",           opts.upBias);
    readFloatField(ctx, obj, "spread",           opts.spread);
    readBoolField (ctx, obj, "includeTwigMesh",  opts.includeTwigMesh);
    readBoolField (ctx, obj, "shapedSilhouette", opts.shapedSilhouette);
    readBoolField (ctx, obj, "fullUV",           opts.fullUV);
}

// ── Species partial application ────────────────────────────────────────

inline void applySpeciesPartial(JSContext* ctx, JSValueConst spec,
                                broflora::Species& s) {
    if (!JS_IsObject(spec)) return;
    readFloatField(ctx, spec, "maxVigor",            s.maxVigor);
    readFloatField(ctx, spec, "minVigor",            s.minVigor);
    readFloatField(ctx, spec, "rootVigorMax",        s.rootVigorMax);
    readFloatField(ctx, spec, "apicalControl",       s.apicalControl);
    readFloatField(ctx, spec, "determinacy",         s.determinacy);
    readFloatField(ctx, spec, "shadeTolerance",      s.shadeTolerance);
    readFloatField(ctx, spec, "apicalControlMature", s.apicalControlMature);
    readFloatField(ctx, spec, "determinacyMature",   s.determinacyMature);
    readVec3Prop  (ctx, spec, "tropismDir",          s.tropismDir);
    readFloatField(ctx, spec, "tropismG1",           s.tropismG1);
    readFloatField(ctx, spec, "tropismG2",           s.tropismG2);
    readFloatField(ctx, spec, "growthScale",         s.growthScale);
    readFloatField(ctx, spec, "climateOptT",         s.climateOptT);
    readFloatField(ctx, spec, "climateOptP",         s.climateOptP);
    readFloatField(ctx, spec, "climateSigT",         s.climateSigT);
    readFloatField(ctx, spec, "climateSigP",         s.climateSigP);
    readFloatField(ctx, spec, "maxAge",              s.maxAge);
    readFloatField(ctx, spec, "floweringAge",        s.floweringAge);
    readFloatField(ctx, spec, "seedingRadius",       s.seedingRadius);
    readFloatField(ctx, spec, "moduleMatureAge",     s.moduleMatureAge);
    readFloatField(ctx, spec, "pipeExp",             s.pipeExp);
    readFloatField(ctx, spec, "leafDiameter",        s.leafDiameter);
    readFloatField(ctx, spec, "terrainAnchorWeight", s.terrainAnchorWeight);
    readFloatField(ctx, spec, "maxSeedingSlope",     s.maxSeedingSlope);
    readFloatField(ctx, spec, "distributionWeightCollisions", s.distributionWeightCollisions);
    readFloatField(ctx, spec, "distributionWeightTropism",    s.distributionWeightTropism);
    readFloatField(ctx, spec, "orthotropy",          s.orthotropy);
    readFloatField(ctx, spec, "individualVariation", s.individualVariation);
}

// ── Prototype builder ──────────────────────────────────────────────────

inline bool buildPrototype(JSContext* ctx, JSValueConst spec,
                           broflora::BranchModulePrototype& out,
                           std::string& nameStorage) {
    if (!JS_IsObject(spec)) return false;

    JSValue nameV = JS_GetPropertyStr(ctx, spec, "name");
    if (JS_IsString(nameV)) {
        size_t len = 0;
        const char* cstr = JS_ToCStringLen(ctx, &len, nameV);
        if (cstr) {
            nameStorage.assign(cstr, len);
            out.name = nameStorage.c_str();
            JS_FreeCString(ctx, cstr);
        }
    }
    JS_FreeValue(ctx, nameV);

    JSValue nodesV = JS_GetPropertyStr(ctx, spec, "nodes");
    if (JS_IsArray(nodesV)) {
        JSValue lenV = JS_GetPropertyStr(ctx, nodesV, "length");
        uint32_t n = 0; JS_ToUint32(ctx, &n, lenV); JS_FreeValue(ctx, lenV);
        out.nodes.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            JSValue nv = JS_GetPropertyUint32(ctx, nodesV, i);
            broflora::ModuleNode mn;
            readVec3Prop  (ctx, nv, "position",    mn.position);
            readFloatField(ctx, nv, "ageAtBirth",  mn.ageAtBirth);
            readFloatField(ctx, nv, "lengthMax",   mn.lengthMax);
            readFloatField(ctx, nv, "thickening",  mn.thickening);
            out.nodes.push_back(mn);
            JS_FreeValue(ctx, nv);
        }
    }
    JS_FreeValue(ctx, nodesV);

    JSValue edgesV = JS_GetPropertyStr(ctx, spec, "edges");
    if (JS_IsArray(edgesV)) {
        JSValue lenV = JS_GetPropertyStr(ctx, edgesV, "length");
        uint32_t n = 0; JS_ToUint32(ctx, &n, lenV); JS_FreeValue(ctx, lenV);
        out.edges.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            JSValue ev = JS_GetPropertyUint32(ctx, edgesV, i);
            broflora::ModuleEdge e{};
            if (JS_IsArray(ev)) {
                JSValue a = JS_GetPropertyUint32(ctx, ev, 0);
                JSValue b = JS_GetPropertyUint32(ctx, ev, 1);
                JS_ToUint32(ctx, &e.a, a);
                JS_ToUint32(ctx, &e.b, b);
                JS_FreeValue(ctx, a); JS_FreeValue(ctx, b);
            } else if (JS_IsObject(ev)) {
                readUint32Field(ctx, ev, "a", e.a);
                readUint32Field(ctx, ev, "b", e.b);
            }
            out.edges.push_back(e);
            JS_FreeValue(ctx, ev);
        }
    }
    JS_FreeValue(ctx, edgesV);

    readUint32Field(ctx, spec, "rootNode", out.rootNode);

    JSValue termsV = JS_GetPropertyStr(ctx, spec, "terminalNodes");
    if (JS_IsArray(termsV)) {
        JSValue lenV = JS_GetPropertyStr(ctx, termsV, "length");
        uint32_t n = 0; JS_ToUint32(ctx, &n, lenV); JS_FreeValue(ctx, lenV);
        out.terminalNodes.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            JSValue v = JS_GetPropertyUint32(ctx, termsV, i);
            uint32_t idx = 0;
            JS_ToUint32(ctx, &idx, v);
            out.terminalNodes.push_back(idx);
            JS_FreeValue(ctx, v);
        }
    }
    JS_FreeValue(ctx, termsV);

    return !out.nodes.empty();
}

// ── Climate / shadow ───────────────────────────────────────────────────

inline void readClimateFields(JSContext* ctx, JSValueConst obj, broflora::GlobalClimate& c) {
    if (!JS_IsObject(obj)) return;
    readFloatField(ctx, obj, "annualTempBase",   c.annualTempBase);
    readFloatField(ctx, obj, "annualPrecip",     c.annualPrecip);
    readFloatField(ctx, obj, "tempLapsePerUnit", c.tempLapsePerUnit);
}

inline void readClimate(JSContext* ctx, JSValueConst opts, broflora::GlobalClimate& c) {
    JSValue cv = JS_GetPropertyStr(ctx, opts, "climate");
    readClimateFields(ctx, cv, c);
    JS_FreeValue(ctx, cv);
}

inline void readShadow(JSContext* ctx, JSValueConst opts, broflora::ShadowGrid& g) {
    JSValue sv = JS_GetPropertyStr(ctx, opts, "shadow");
    if (JS_IsObject(sv)) {
        readVec3Prop(ctx, sv, "origin", g.origin);
        readFloatField(ctx, sv, "cellSize", g.cellSize);
        readUint32Field(ctx, sv, "width",  g.width);
        readUint32Field(ctx, sv, "height", g.height);
        readUint32Field(ctx, sv, "depth",  g.depth);
        float fill = 1.0f;
        readFloatField(ctx, sv, "fill", fill);
        const size_t n = (size_t)g.width * g.height * g.depth;
        g.qg.assign(n, fill);
    }
    JS_FreeValue(ctx, sv);
}

inline JSValue protoToSpec(JSContext* ctx, const broflora::BranchModulePrototype& p) {
    JSValue o = JS_NewObject(ctx);
    if (p.name) JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, p.name));

    JSValue nodes = JS_NewArray(ctx);
    for (uint32_t i = 0; i < p.nodes.size(); ++i) {
        const auto& nd = p.nodes[i];
        JSValue nv = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, nv, "position",   makeVec3(ctx, nd.position));
        JS_SetPropertyStr(ctx, nv, "ageAtBirth", JS_NewFloat64(ctx, nd.ageAtBirth));
        JS_SetPropertyStr(ctx, nv, "lengthMax",  JS_NewFloat64(ctx, nd.lengthMax));
        JS_SetPropertyStr(ctx, nv, "thickening", JS_NewFloat64(ctx, nd.thickening));
        JS_SetPropertyUint32(ctx, nodes, i, nv);
    }
    JS_SetPropertyStr(ctx, o, "nodes", nodes);

    JSValue edges = JS_NewArray(ctx);
    for (uint32_t i = 0; i < p.edges.size(); ++i) {
        JSValue ev = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, ev, 0, JS_NewInt32(ctx, (int32_t)p.edges[i].a));
        JS_SetPropertyUint32(ctx, ev, 1, JS_NewInt32(ctx, (int32_t)p.edges[i].b));
        JS_SetPropertyUint32(ctx, edges, i, ev);
    }
    JS_SetPropertyStr(ctx, o, "edges", edges);
    JS_SetPropertyStr(ctx, o, "rootNode", JS_NewInt32(ctx, (int32_t)p.rootNode));

    JSValue terms = JS_NewArray(ctx);
    for (uint32_t i = 0; i < p.terminalNodes.size(); ++i)
        JS_SetPropertyUint32(ctx, terms, i, JS_NewInt32(ctx, (int32_t)p.terminalNodes[i]));
    JS_SetPropertyStr(ctx, o, "terminalNodes", terms);
    return o;
}

void registerFloraWorldEmitMethods(qjsbind::Class<FWW>& cls);

} // namespace bro::js

#endif // BRO_WITH_FLORA
