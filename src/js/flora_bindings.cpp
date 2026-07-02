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
    readFloatField(ctx, spec, "orthotropy",          s.orthotropy);
    readFloatField(ctx, spec, "individualVariation", s.individualVariation);
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

// Read climate fields straight off an object (used by setClimate).
static void readClimateFields(JSContext* ctx, JSValueConst obj, broflora::GlobalClimate& c) {
    if (!JS_IsObject(obj)) return;
    readFloatField(ctx, obj, "annualTempBase",   c.annualTempBase);
    readFloatField(ctx, obj, "annualPrecip",     c.annualPrecip);
    readFloatField(ctx, obj, "tempLapsePerUnit", c.tempLapsePerUnit);
}

static void readClimate(JSContext* ctx, JSValueConst opts, broflora::GlobalClimate& c) {
    JSValue cv = JS_GetPropertyStr(ctx, opts, "climate");
    readClimateFields(ctx, cv, c);
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

// ── Built-in prototypes ────────────────────────────────────────────────
// Serialise a C++ broflora prototype into the same spec object
// buildPrototype() reads, so the library factories stay the single source
// of truth and the returned value drops straight into world.addPrototype().
static JSValue protoToSpec(JSContext* ctx, const broflora::BranchModulePrototype& p) {
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

static JSValue protoStraight(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return protoToSpec(ctx, broflora::straightModule());
}
static JSValue protoFork(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return protoToSpec(ctx, broflora::forkModule());
}
static JSValue protoWhorl(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    uint32_t arms = 3; double spread = 0.55;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) JS_ToUint32(ctx, &arms, argv[0]);
    if (argc >= 2 && !JS_IsUndefined(argv[1])) JS_ToFloat64(ctx, &spread, argv[1]);
    return protoToSpec(ctx, broflora::whorlModule(arms, (float)spread));
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

    // -- remove plant (swap-and-pop) --
    // Plant indices are not stable across removePlant or step — the
    // sim's own senescence pass already invalidates indices when a
    // plant fully dies. Callers must re-fetch indices after either.
    .method("removePlant", [](FWW* w, int plantIdx) -> bool {
        if (plantIdx < 0) return false;
        return broflora::removePlant(*w->world, (uint32_t)plantIdx);
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
            JS_SetPropertyStr(ctx, o, "lightExposure01", JS_NewFloat64(ctx, s.lightExposure01));
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
            JS_SetPropertyStr(ctx, o, "lightExposure01", JS_NewFloat64(ctx, a.lightExposure01));
            JS_SetPropertyStr(ctx, o, "senescence01", JS_NewFloat64(ctx, a.senescence01));
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
        }
        return arr;
    })

    // -- per-plant emit (parallel to the world-level ones above) -----------
    // All four return null when plantIdx is out of range; the array forms
    // return an empty array for valid-but-empty plants (no segments yet,
    // pre-flowering for blooms, etc).

    .method("emitPlantMesh", [](FWW* w, JSContext* ctx, int plantIdx, int sides) -> JSValue {
        if (plantIdx < 0 || (size_t)plantIdx >= w->world->plants.size()) return JS_NULL;
        uint32_t s = (sides >= 3) ? (uint32_t)sides : 6u;
        auto md = std::make_unique<bromesh::MeshData>(
            broflora::emitPlantMesh(w->world->plants[(size_t)plantIdx], s));
        return MeshBindings::wrapMeshData(ctx, std::move(md));
    })

    .method("emitPlantSegments", [](FWW* w, JSContext* ctx, int plantIdx) -> JSValue {
        if (plantIdx < 0 || (size_t)plantIdx >= w->world->plants.size()) return JS_NULL;
        auto segs = broflora::emitPlantSegments(w->world->plants[(size_t)plantIdx]);
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

    .method("emitPlantFoliage", [](FWW* w, JSContext* ctx, int plantIdx) -> JSValue {
        if (plantIdx < 0 || (size_t)plantIdx >= w->world->plants.size()) return JS_NULL;
        auto samples = broflora::emitPlantFoliage(w->world->plants[(size_t)plantIdx]);
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < samples.size(); ++i) {
            const auto& s = samples[i];
            JSValue o = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, o, "mass",         JS_NewFloat64(ctx, s.mass));
            JS_SetPropertyStr(ctx, o, "age01",        JS_NewFloat64(ctx, s.age01));
            JS_SetPropertyStr(ctx, o, "vigor01",      JS_NewFloat64(ctx, s.vigor01));
            JS_SetPropertyStr(ctx, o, "light01",      JS_NewFloat64(ctx, s.light01));
            JS_SetPropertyStr(ctx, o, "lightExposure01", JS_NewFloat64(ctx, s.lightExposure01));
            JS_SetPropertyStr(ctx, o, "senescence01", JS_NewFloat64(ctx, s.senescence01));
            JS_SetPropertyStr(ctx, o, "isTerminal",   JS_NewBool(ctx, s.isTerminal));
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
        }
        return arr;
    })

    .method("emitPlantBloomAnchors", [](FWW* w, JSContext* ctx, int plantIdx) -> JSValue {
        if (plantIdx < 0 || (size_t)plantIdx >= w->world->plants.size()) return JS_NULL;
        auto anchors = broflora::emitPlantBloomAnchors(w->world->plants[(size_t)plantIdx]);
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < anchors.size(); ++i) {
            const auto& a = anchors[i];
            JSValue o = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, o, "position",     makeVec3(ctx, a.position));
            JS_SetPropertyStr(ctx, o, "normal",       makeVec3(ctx, a.normal));
            JS_SetPropertyStr(ctx, o, "age01",        JS_NewFloat64(ctx, a.age01));
            JS_SetPropertyStr(ctx, o, "vigor01",      JS_NewFloat64(ctx, a.vigor01));
            JS_SetPropertyStr(ctx, o, "lightExposure01", JS_NewFloat64(ctx, a.lightExposure01));
            JS_SetPropertyStr(ctx, o, "senescence01", JS_NewFloat64(ctx, a.senescence01));
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
        }
        return arr;
    })

    // -- per-plant inspect -------------------------------------------------
    // Snapshot of a plant's runtime state + a copy of every Species field.
    // Useful for inspector panels and per-species partitioning JS-side.
    .method("plantInfo", [](FWW* w, JSContext* ctx, int plantIdx) -> JSValue {
        if (plantIdx < 0 || (size_t)plantIdx >= w->world->plants.size()) return JS_NULL;
        const auto& p = w->world->plants[(size_t)plantIdx];

        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "origin",                 makeVec3(ctx, p.origin));
        JS_SetPropertyStr(ctx, o, "age",                    JS_NewFloat64(ctx, p.age));
        JS_SetPropertyStr(ctx, o, "flowering",              JS_NewBool   (ctx, p.flowering));
        JS_SetPropertyStr(ctx, o, "senescing",              JS_NewBool   (ctx, p.senescing));
        JS_SetPropertyStr(ctx, o, "moduleCount",            JS_NewInt32  (ctx, (int32_t)p.modules.size()));
        JS_SetPropertyStr(ctx, o, "effectiveRootVigorMax",  JS_NewFloat64(ctx, p.effectiveRootVigorMax));
        if (!p.modules.empty()) {
            JS_SetPropertyStr(ctx, o, "rootVigor", JS_NewFloat64(ctx, p.modules.front().vigor));
            JS_SetPropertyStr(ctx, o, "rootLight", JS_NewFloat64(ctx, p.modules.front().light));
        }

        // Species — full snapshot. Kept flat to mirror addPlant's spec shape.
        const auto& s = p.species;
        JSValue sp = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, sp, "maxVigor",                    JS_NewFloat64(ctx, s.maxVigor));
        JS_SetPropertyStr(ctx, sp, "minVigor",                    JS_NewFloat64(ctx, s.minVigor));
        JS_SetPropertyStr(ctx, sp, "rootVigorMax",                JS_NewFloat64(ctx, s.rootVigorMax));
        JS_SetPropertyStr(ctx, sp, "apicalControl",               JS_NewFloat64(ctx, s.apicalControl));
        JS_SetPropertyStr(ctx, sp, "determinacy",                 JS_NewFloat64(ctx, s.determinacy));
        JS_SetPropertyStr(ctx, sp, "shadeTolerance",              JS_NewFloat64(ctx, s.shadeTolerance));
        JS_SetPropertyStr(ctx, sp, "apicalControlMature",         JS_NewFloat64(ctx, s.apicalControlMature));
        JS_SetPropertyStr(ctx, sp, "determinacyMature",           JS_NewFloat64(ctx, s.determinacyMature));
        JS_SetPropertyStr(ctx, sp, "tropismDir",                  makeVec3     (ctx, s.tropismDir));
        JS_SetPropertyStr(ctx, sp, "tropismG1",                   JS_NewFloat64(ctx, s.tropismG1));
        JS_SetPropertyStr(ctx, sp, "tropismG2",                   JS_NewFloat64(ctx, s.tropismG2));
        JS_SetPropertyStr(ctx, sp, "growthScale",                 JS_NewFloat64(ctx, s.growthScale));
        JS_SetPropertyStr(ctx, sp, "climateOptT",                 JS_NewFloat64(ctx, s.climateOptT));
        JS_SetPropertyStr(ctx, sp, "climateOptP",                 JS_NewFloat64(ctx, s.climateOptP));
        JS_SetPropertyStr(ctx, sp, "climateSigT",                 JS_NewFloat64(ctx, s.climateSigT));
        JS_SetPropertyStr(ctx, sp, "climateSigP",                 JS_NewFloat64(ctx, s.climateSigP));
        JS_SetPropertyStr(ctx, sp, "maxAge",                      JS_NewFloat64(ctx, s.maxAge));
        JS_SetPropertyStr(ctx, sp, "floweringAge",                JS_NewFloat64(ctx, s.floweringAge));
        JS_SetPropertyStr(ctx, sp, "seedingRadius",               JS_NewFloat64(ctx, s.seedingRadius));
        JS_SetPropertyStr(ctx, sp, "moduleMatureAge",             JS_NewFloat64(ctx, s.moduleMatureAge));
        JS_SetPropertyStr(ctx, sp, "pipeExp",                     JS_NewFloat64(ctx, s.pipeExp));
        JS_SetPropertyStr(ctx, sp, "leafDiameter",                JS_NewFloat64(ctx, s.leafDiameter));
        JS_SetPropertyStr(ctx, sp, "terrainAnchorWeight",         JS_NewFloat64(ctx, s.terrainAnchorWeight));
        JS_SetPropertyStr(ctx, sp, "maxSeedingSlope",             JS_NewFloat64(ctx, s.maxSeedingSlope));
        JS_SetPropertyStr(ctx, sp, "distributionWeightCollisions",JS_NewFloat64(ctx, s.distributionWeightCollisions));
        JS_SetPropertyStr(ctx, sp, "distributionWeightTropism",   JS_NewFloat64(ctx, s.distributionWeightTropism));
        JS_SetPropertyStr(ctx, sp, "orthotropy",                  JS_NewFloat64(ctx, s.orthotropy));
        JS_SetPropertyStr(ctx, sp, "individualVariation",         JS_NewFloat64(ctx, s.individualVariation));
        JS_SetPropertyStr(ctx, o, "species", sp);

        return o;
    })

    // -- climate mutation (no reset required) ------------------------------
    // Climate change directly drives the species adaptation factor σ each
    // tick, so a slider can show succession in real time.
    .method("setClimate", [](FWW* w, JSContext* ctx, JSValue opts) {
        readClimateFields(ctx, opts, w->world->climate);
    }, qjsbind::returns_this)

    // -- shadow grid read --------------------------------------------------
    // Returns the cell-centered Q_G at world-space [x,y,z], or null if the
    // grid has no cells or the sample falls outside. Nearest-cell lookup —
    // matches what the sim itself reads when computing light per module.
    .method("sampleShadow", [](FWW* w, JSContext* ctx, JSValue posV) -> JSValue {
        const auto& g = w->world->shadow;
        if (g.qg.empty() || g.width == 0 || g.height == 0 || g.depth == 0) return JS_NULL;
        bromath::Vec3 p{};
        if (!JS_IsArray(posV)) return JS_NULL;
        for (int i = 0; i < 3; ++i) {
            JSValue el = JS_GetPropertyUint32(ctx, posV, (uint32_t)i);
            double d = 0; JS_ToFloat64(ctx, &d, el); JS_FreeValue(ctx, el);
            (&p.x)[i] = (float)d;
        }
        const float inv = (g.cellSize > 0.0f) ? 1.0f / g.cellSize : 0.0f;
        int ix = (int)((p.x - g.origin.x) * inv);
        int iy = (int)((p.y - g.origin.y) * inv);
        int iz = (int)((p.z - g.origin.z) * inv);
        if (ix < 0 || iy < 0 || iz < 0) return JS_NULL;
        if ((uint32_t)ix >= g.width || (uint32_t)iy >= g.height || (uint32_t)iz >= g.depth) return JS_NULL;
        const uint32_t idx = broflora::shadowIndex(g, (uint32_t)ix, (uint32_t)iy, (uint32_t)iz);
        return JS_NewFloat64(ctx, g.qg[idx]);
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

    // bro.flora.prototypes.{straight,fork,whorl} — ready-made specs that
    // drop straight into world.addPrototype(...).
    JSValue protos = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, protos, "straight",
                      JS_NewCFunction(ctx, protoStraight, "straight", 0));
    JS_SetPropertyStr(ctx, protos, "fork",
                      JS_NewCFunction(ctx, protoFork, "fork", 0));
    JS_SetPropertyStr(ctx, protos, "whorl",
                      JS_NewCFunction(ctx, protoWhorl, "whorl", 2));
    JS_SetPropertyStr(ctx, floraObj, "prototypes", protos);

    JS_FreeValue(ctx, floraObj);
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

} // namespace bro::js
