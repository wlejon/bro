#if BRO_WITH_FLORA

#include "js/flora_bindings.h"
#include "js/flora_bindings_internal.h"
#include "js/mesh_bindings.h"
#include <broflora/broflora.h>
#include <bromath/vec.h>
#include <bromesh/mesh_data.h>
#include <bromesh/procedural/leaf_scatter.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

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

static JSValue protoMonopodial(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    uint32_t lateralBranches = 2; double lateralSpread = 0.7;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) JS_ToUint32(ctx, &lateralBranches, argv[0]);
    if (argc >= 2 && !JS_IsUndefined(argv[1])) JS_ToFloat64(ctx, &lateralSpread, argv[1]);
    return protoToSpec(ctx, broflora::monopodialLeaderModule(lateralBranches, (float)lateralSpread));
}

static JSValue protoSympodial(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    double primarySpread = 0.3; double lateralSpread = 0.7;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) JS_ToFloat64(ctx, &primarySpread, argv[0]);
    if (argc >= 2 && !JS_IsUndefined(argv[1])) JS_ToFloat64(ctx, &lateralSpread, argv[1]);
    return protoToSpec(ctx, broflora::sympodialForkModule((float)primarySpread, (float)lateralSpread));
}

static JSValue protoHorizontalTier(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    uint32_t arms = 3; double spread = 0.85;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) JS_ToUint32(ctx, &arms, argv[0]);
    if (argc >= 2 && !JS_IsUndefined(argv[1])) JS_ToFloat64(ctx, &spread, argv[1]);
    return protoToSpec(ctx, broflora::horizontalTierModule(arms, (float)spread));
}

static JSValue protoWeeping(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    double spread = 0.6; double droop = 0.4;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) JS_ToFloat64(ctx, &spread, argv[0]);
    if (argc >= 2 && !JS_IsUndefined(argv[1])) JS_ToFloat64(ctx, &droop, argv[1]);
    return protoToSpec(ctx, broflora::weepingModule((float)spread, (float)droop));
}

static JSValue jsLeafCluster(JSContext* ctx, JSValueConst,
                             int argc, JSValueConst* argv) {
    broflora::Phyllotaxy phyl = broflora::Phyllotaxy::Alternate;
    broflora::LeafClusterOptions opts;
    if (argc >= 1) {
        if (JS_IsObject(argv[0]) && !JS_IsNumber(argv[0]) && !JS_IsString(argv[0])) {
            readLeafClusterOptions(ctx, argv[0], opts);
            JSValue pv = JS_GetPropertyStr(ctx, argv[0], "phyllotaxy");
            if (!JS_IsUndefined(pv) && !JS_IsNull(pv)) {
                phyl = parsePhyllotaxy(ctx, pv);
            }
            JS_FreeValue(ctx, pv);
        } else {
            phyl = parsePhyllotaxy(ctx, argv[0]);
            if (argc >= 2 && JS_IsObject(argv[1])) {
                readLeafClusterOptions(ctx, argv[1], opts);
            }
        }
    }
    auto md = std::make_unique<bromesh::MeshData>(broflora::leafCluster(phyl, opts));
    return MeshBindings::wrapMeshData(ctx, std::move(md));
}

static JSValue createWorld(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto world = std::make_unique<broflora::WorldState>();
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];
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

static void installWorldClass(JSContext* ctx) {
    qjsbind::Class<FWW> cls(ctx, "FloraWorld", qjsbind::NoGlobal);
    registerFloraWorldEmitMethods(cls);
    cls
    .method("addPrototype", [](FWW* w, JSContext* ctx, JSValue spec) -> int {
        broflora::BranchModulePrototype proto;
        std::string nameStorage;
        if (!buildPrototype(ctx, spec, proto, nameStorage))
            return -1;
        return (int)broflora::addPrototype(*w->world, std::move(proto));
    })
    .method("addVoronoiSite", [](FWW* w, int prototypeIndex,
                                 double determinacy, double apicalControl) {
        broflora::addVoronoiSite(*w->world, (uint32_t)prototypeIndex,
                                 (float)determinacy, (float)apicalControl);
    }, qjsbind::returns_this)
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
    .method("removePlant", [](FWW* w, int plantIdx) -> bool {
        if (plantIdx < 0) return false;
        return broflora::removePlant(*w->world, (uint32_t)plantIdx);
    })
    .method("step", [](FWW* w, double dt) {
        broflora::step(*w->world, (float)dt);
    }, qjsbind::returns_this)
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
    .method("setClimate", [](FWW* w, JSContext* ctx, JSValue opts) {
        readClimateFields(ctx, opts, w->world->climate);
    }, qjsbind::returns_this)
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
    .method("validate", [](FWW* w, JSContext* ctx) -> JSValue {
        std::string err;
        if (broflora::validate(*w->world, &err)) return JS_NULL;
        return JS_NewStringLen(ctx, err.data(), err.size());
    })
    .get("simTime",         [](FWW* w) -> double { return w->world->simTime; })
    .get("plantCount",      [](FWW* w) -> int { return (int)w->world->plants.size(); })
    .get("prototypeCount",  [](FWW* w) -> int { return (int)w->world->prototypes.size(); })
    .get("moduleCount",     [](FWW* w) -> int {
        size_t total = 0;
        for (const auto& p : w->world->plants) total += p.modules.size();
        return (int)total;
    });
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void FloraBindings::install(JSContext* ctx) {
    installWorldClass(ctx);
    
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
        JS_SetPropertyStr(ctx, floraObj, "leafCluster",
                          JS_NewCFunction(ctx, jsLeafCluster, "leafCluster", 2));
    
        JSValue phylObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, phylObj, "alternate",       JS_NewInt32(ctx, (int32_t)broflora::Phyllotaxy::Alternate));
        JS_SetPropertyStr(ctx, phylObj, "opposite",        JS_NewInt32(ctx, (int32_t)broflora::Phyllotaxy::Opposite));
        JS_SetPropertyStr(ctx, phylObj, "spiral",          JS_NewInt32(ctx, (int32_t)broflora::Phyllotaxy::Spiral));
        JS_SetPropertyStr(ctx, phylObj, "fascicle",        JS_NewInt32(ctx, (int32_t)broflora::Phyllotaxy::Fascicle));
        JS_SetPropertyStr(ctx, phylObj, "compoundPinnate", JS_NewInt32(ctx, (int32_t)broflora::Phyllotaxy::CompoundPinnate));
        JS_SetPropertyStr(ctx, phylObj, "Alternate",       JS_NewInt32(ctx, (int32_t)broflora::Phyllotaxy::Alternate));
        JS_SetPropertyStr(ctx, phylObj, "Opposite",        JS_NewInt32(ctx, (int32_t)broflora::Phyllotaxy::Opposite));
        JS_SetPropertyStr(ctx, phylObj, "Spiral",          JS_NewInt32(ctx, (int32_t)broflora::Phyllotaxy::Spiral));
        JS_SetPropertyStr(ctx, phylObj, "Fascicle",        JS_NewInt32(ctx, (int32_t)broflora::Phyllotaxy::Fascicle));
        JS_SetPropertyStr(ctx, phylObj, "CompoundPinnate", JS_NewInt32(ctx, (int32_t)broflora::Phyllotaxy::CompoundPinnate));
        JS_SetPropertyStr(ctx, floraObj, "phyllotaxy", JS_DupValue(ctx, phylObj));
        JS_SetPropertyStr(ctx, floraObj, "Phyllotaxy", phylObj);
    
        JSValue protos = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, protos, "straight",
                          JS_NewCFunction(ctx, protoStraight, "straight", 0));
        JS_SetPropertyStr(ctx, protos, "fork",
                          JS_NewCFunction(ctx, protoFork, "fork", 0));
        JS_SetPropertyStr(ctx, protos, "whorl",
                          JS_NewCFunction(ctx, protoWhorl, "whorl", 2));
        JS_SetPropertyStr(ctx, protos, "monopodial",
                          JS_NewCFunction(ctx, protoMonopodial, "monopodial", 2));
        JS_SetPropertyStr(ctx, protos, "sympodial",
                          JS_NewCFunction(ctx, protoSympodial, "sympodial", 2));
        JS_SetPropertyStr(ctx, protos, "horizontalTier",
                          JS_NewCFunction(ctx, protoHorizontalTier, "horizontalTier", 2));
        JS_SetPropertyStr(ctx, protos, "tier",
                          JS_NewCFunction(ctx, protoHorizontalTier, "tier", 2));
        JS_SetPropertyStr(ctx, protos, "weeping",
                          JS_NewCFunction(ctx, protoWeeping, "weeping", 2));
        JS_SetPropertyStr(ctx, floraObj, "prototypes", protos);
    
        JS_FreeValue(ctx, floraObj);
        JS_FreeValue(ctx, broObj);
        JS_FreeValue(ctx, global);
}


} // namespace bro::js

#endif // BRO_WITH_FLORA
