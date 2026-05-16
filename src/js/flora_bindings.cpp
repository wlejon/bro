#include "js/flora_bindings.h"
#include "js/mesh_bindings.h"

#include <qjsbind/qjsbind.h>

#include <broflora/broflora.h>

#include <bromath/vec.h>
#include <bromesh/mesh_data.h>

#include <cstdint>
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

// ── Vec3 helpers ───────────────────────────────────────────────────────

static bool readVec3Prop(JSContext* ctx, JSValueConst obj, const char* prop,
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

static JSValue makeVec3(JSContext* ctx, const bromath::Vec3& v) {
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, v.x));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, v.y));
    JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, v.z));
    return arr;
}

// ── Optional-field readers ─────────────────────────────────────────────

static bool readFloatField(JSContext* ctx, JSValueConst obj, const char* prop, float& out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return false; }
    double d = 0;
    if (JS_ToFloat64(ctx, &d, v) == 0) out = (float)d;
    JS_FreeValue(ctx, v);
    return true;
}

static bool readUint32Field(JSContext* ctx, JSValueConst obj, const char* prop, uint32_t& out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return false; }
    uint32_t u = 0;
    if (JS_ToUint32(ctx, &u, v) == 0) out = u;
    JS_FreeValue(ctx, v);
    return true;
}

// ── Species partial application ────────────────────────────────────────
// Caller-supplied fields override defaults; missing fields keep Species'
// in-class defaults.
static void applySpeciesPartial(JSContext* ctx, JSValueConst spec,
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
    readFloatField(ctx, spec, "tropismCosTarget",    s.tropismCosTarget);
}

// ── Prototype builder ──────────────────────────────────────────────────
//   spec: { name?, nodes: [{position:[x,y,z], ageAtBirth?, lengthMax?, thickening?}, ...],
//           edges: [[a,b], ...] | [{a,b}, ...], rootNode?, terminalNodes: [idx, ...] }
static bool buildPrototype(JSContext* ctx, JSValueConst spec,
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

static void readClimate(JSContext* ctx, JSValueConst opts, broflora::GlobalClimate& c) {
    JSValue cv = JS_GetPropertyStr(ctx, opts, "climate");
    if (JS_IsObject(cv)) {
        readFloatField(ctx, cv, "annualTempBase",   c.annualTempBase);
        readFloatField(ctx, cv, "annualPrecip",     c.annualPrecip);
        readFloatField(ctx, cv, "tempLapsePerUnit", c.tempLapsePerUnit);
    }
    JS_FreeValue(ctx, cv);
}

static void readShadow(JSContext* ctx, JSValueConst opts, broflora::ShadowGrid& g) {
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

// ── Factory ────────────────────────────────────────────────────────────

static JSValue createWorld(JSContext* ctx, JSValueConst /*this_val*/,
                           int argc, JSValueConst* argv) {
    auto world = std::make_unique<broflora::WorldState>();

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];

        // Rng seed (optional).
        JSValue seedV = JS_GetPropertyStr(ctx, opts, "rngSeed");
        if (!JS_IsUndefined(seedV) && !JS_IsNull(seedV)) {
            int64_t seed = 0;
            if (JS_ToInt64(ctx, &seed, seedV) == 0)
                world->rngState = (uint64_t)seed;
        }
        JS_FreeValue(ctx, seedV);

        readClimate(ctx, opts, world->climate);
        readShadow (ctx, opts, world->shadow);
    }

    auto* wrap = new FWW{std::move(world)};
    return qjsbind::wrap<FWW>(ctx, wrap);
}

// ── Class registration ─────────────────────────────────────────────────

static void installWorldClass(JSContext* ctx) {
    qjsbind::Class<FWW>(ctx, "FloraWorld", qjsbind::NoGlobal)

    // -- prototype builder --
    .method("addPrototype", [](FWW* w, JSContext* ctx, JSValue spec) -> int {
        broflora::BranchModulePrototype proto;
        std::string nameStorage;
        if (!buildPrototype(ctx, spec, proto, nameStorage))
            return -1;
        return (int)broflora::addPrototype(*w->world, std::move(proto));
    })

    // -- voronoi site --
    .method("addVoronoiSite", [](FWW* w, int prototypeIndex,
                                 double determinacy, double apicalControl) {
        broflora::addVoronoiSite(*w->world, (uint32_t)prototypeIndex,
                                 (float)determinacy, (float)apicalControl);
    }, qjsbind::returns_this)

    // -- plant --
    //  spec: { origin:[x,y,z], species?, prototypeIndex?, initialVigor?, age? }
    .method("addPlant", [](FWW* w, JSContext* ctx, JSValue spec) -> int {
        if (!JS_IsObject(spec)) return -1;

        broflora::Plant p;
        p.species = {};
        JSValue speciesV = JS_GetPropertyStr(ctx, spec, "species");
        applySpeciesPartial(ctx, speciesV, p.species);
        JS_FreeValue(ctx, speciesV);

        readVec3Prop(ctx, spec, "origin", p.origin);
        readFloatField(ctx, spec, "age", p.age);
        p.effectiveRootVigorMax = p.species.rootVigorMax;

        // Initial root module — created from a registered prototype index.
        uint32_t protoIdx = UINT32_MAX;
        if (readUint32Field(ctx, spec, "prototypeIndex", protoIdx)) {
            const auto* proto = broflora::prototypeAt(*w->world, protoIdx);
            if (!proto) return -1;
            broflora::BranchModuleInstance root;
            root.prototype = proto;
            root.parent    = UINT32_MAX;
            root.age       = 0.0f;
            float initialVigor = p.species.minVigor * 2.0f;
            readFloatField(ctx, spec, "initialVigor", initialVigor);
            root.vigor = initialVigor;
            root.light = 1.0f;
            p.modules.push_back(root);
        }

        broflora::addPlant(*w->world, std::move(p));
        return (int)(w->world->plants.size() - 1);
    })

    // -- tick --
    .method("step", [](FWW* w, double dt) {
        broflora::step(*w->world, (float)dt);
    }, qjsbind::returns_this)

    // -- mesh emit (returns JS Mesh) --
    .method("emitMesh", [](FWW* w, JSContext* ctx, int sides) -> JSValue {
        uint32_t s = (sides >= 3) ? (uint32_t)sides : 6u;
        auto md = std::make_unique<bromesh::MeshData>(
            broflora::emitWorldMesh(*w->world, s));
        return MeshBindings::wrapMeshData(ctx, std::move(md));
    })

    // -- segments --
    .method("emitSegments", [](FWW* w, JSContext* ctx) -> JSValue {
        auto segs = broflora::emitWorldSegments(*w->world);
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < segs.size(); ++i) {
            const auto& s = segs[i];
            JSValue o = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, o, "from",   makeVec3(ctx, s.from));
            JS_SetPropertyStr(ctx, o, "to",     makeVec3(ctx, s.to));
            JS_SetPropertyStr(ctx, o, "radius", JS_NewFloat64(ctx, s.radius));
            JS_SetPropertyStr(ctx, o, "depth",  JS_NewInt32(ctx, (int32_t)s.depth));
            JS_SetPropertyStr(ctx, o, "parent", JS_NewInt32(ctx, (int32_t)s.parent));
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
        }
        return arr;
    })

    // -- foliage samples --
    .method("emitFoliage", [](FWW* w, JSContext* ctx) -> JSValue {
        auto samples = broflora::emitWorldFoliage(*w->world);
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < samples.size(); ++i) {
            const auto& s = samples[i];
            JSValue o = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, o, "mass",         JS_NewFloat64(ctx, s.mass));
            JS_SetPropertyStr(ctx, o, "age01",        JS_NewFloat64(ctx, s.age01));
            JS_SetPropertyStr(ctx, o, "vigor01",      JS_NewFloat64(ctx, s.vigor01));
            JS_SetPropertyStr(ctx, o, "light01",      JS_NewFloat64(ctx, s.light01));
            JS_SetPropertyStr(ctx, o, "senescence01", JS_NewFloat64(ctx, s.senescence01));
            JS_SetPropertyStr(ctx, o, "isTerminal",   JS_NewBool(ctx, s.isTerminal));
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
        }
        return arr;
    })

    // -- bloom anchors --
    .method("emitBloomAnchors", [](FWW* w, JSContext* ctx) -> JSValue {
        auto anchors = broflora::emitWorldBloomAnchors(*w->world);
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < anchors.size(); ++i) {
            const auto& a = anchors[i];
            JSValue o = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, o, "position",     makeVec3(ctx, a.position));
            JS_SetPropertyStr(ctx, o, "normal",       makeVec3(ctx, a.normal));
            JS_SetPropertyStr(ctx, o, "age01",        JS_NewFloat64(ctx, a.age01));
            JS_SetPropertyStr(ctx, o, "vigor01",      JS_NewFloat64(ctx, a.vigor01));
            JS_SetPropertyStr(ctx, o, "senescence01", JS_NewFloat64(ctx, a.senescence01));
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
        }
        return arr;
    })

    // -- validation --
    .method("validate", [](FWW* w, JSContext* ctx) -> JSValue {
        std::string err;
        if (broflora::validate(*w->world, &err)) return JS_NULL;
        return JS_NewStringLen(ctx, err.data(), err.size());
    })

    // -- read-only state --
    .get("simTime",         [](FWW* w) -> double { return w->world->simTime; })
    .get("plantCount",      [](FWW* w) -> int { return (int)w->world->plants.size(); })
    .get("prototypeCount",  [](FWW* w) -> int { return (int)w->world->prototypes.size(); })
    .get("moduleCount",     [](FWW* w) -> int {
        size_t total = 0;
        for (const auto& p : w->world->plants) total += p.modules.size();
        return (int)total;
    })
    ;
}

void FloraBindings::install(JSContext* ctx) {
    installWorldClass(ctx);

    // bro.flora.* namespace.
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }
    JSValue floraObj = JS_GetPropertyStr(ctx, broObj, "flora");
    if (JS_IsUndefined(floraObj) || JS_IsException(floraObj)) {
        JS_FreeValue(ctx, floraObj);
        floraObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, broObj, "flora", JS_DupValue(ctx, floraObj));
    }

    JSValue createFn = JS_NewCFunction(ctx, createWorld, "createWorld", 1);
    JS_SetPropertyStr(ctx, floraObj, "createWorld", createFn);

    JS_FreeValue(ctx, floraObj);
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

} // namespace bro::js
