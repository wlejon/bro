// broflora geometry & foliage emit JS bindings (bro.flora.*).
// Compiled only when BRO_WITH_FLORA is on.
#include "js/flora_bindings_internal.h"
#if BRO_WITH_FLORA

#include "js/mesh_bindings.h"

namespace bro::js {

void registerFloraWorldEmitMethods(qjsbind::Class<FWW>& cls) {
    cls
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
            JS_SetPropertyStr(ctx, o, "twigGrade01", JS_NewFloat64(ctx, s.twigGrade01));
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
        }
        return arr;
    })

    // -- fast native C++ foliage mesh emitter --
    .method("emitFoliageMesh", [](FWW* w, JSContext* ctx, JSValueConst leafVal, JSValueConst optsVal) -> JSValue {
        auto* lw = MeshBindings::getMeshData(ctx, leafVal);
        if (!lw || lw->empty()) return JS_ThrowTypeError(ctx, "emitFoliageMesh requires valid leaf Mesh");

        auto segs = broflora::emitWorldSegments(*w->world);
        if (segs.empty()) {
            return MeshBindings::wrapMeshData(ctx, std::make_unique<bromesh::MeshData>());
        }
        auto samples = broflora::emitWorldFoliage(*w->world);

        bromesh::LeafPlacementOptions opts;
        if (JS_IsObject(optsVal)) readLeafPlacementOptions(ctx, optsVal, opts);

        fillFoliageDensity(samples, segs.size(), opts);

        auto md = std::make_unique<bromesh::MeshData>(
            bromesh::scatterLeaves(segs, *lw, opts));
        return MeshBindings::wrapMeshData(ctx, std::move(md));
    })

    // -- fast native C++ foliage transform buffer for InstancedMeshNode --
    .method("emitFoliageTransforms", [](FWW* w, JSContext* ctx, JSValueConst optsVal) -> JSValue {
        auto segs = broflora::emitWorldSegments(*w->world);
        if (segs.empty()) {
            return makeFloat32Array(ctx, nullptr, 0);
        }
        auto samples = broflora::emitWorldFoliage(*w->world);

        bromesh::LeafPlacementOptions opts;
        if (JS_IsObject(optsVal)) readLeafPlacementOptions(ctx, optsVal, opts);

        fillFoliageDensity(samples, segs.size(), opts);

        auto pl = bromesh::placeLeavesOnBranches(segs, opts);
        size_t count = pl.transforms.size();
        if (count == 0) {
            return makeFloat32Array(ctx, nullptr, 0);
        }
        return makeFloat32Array(ctx, pl.transforms.data(), count);
    })

    // -- GPU foliage scatter: pack per-segment records for foliage_scatter.vert --
    .method("emitScatterSegments", [](FWW* w, JSContext* ctx, JSValueConst optsVal) -> JSValue {
        auto segs = broflora::emitWorldSegments(*w->world);
        auto samples = broflora::emitWorldFoliage(*w->world);

        bromesh::LeafPlacementOptions opts;
        if (JS_IsObject(optsVal)) readLeafPlacementOptions(ctx, optsVal, opts);
        fillFoliageDensity(samples, segs.size(), opts);

        // Child counts for the terminalOnly filter (leaves only on chain tips).
        std::vector<int> childCount(segs.size(), 0);
        for (size_t i = 0; i < segs.size(); ++i) {
            int p = segs[i].parent;
            if (p >= 0 && static_cast<size_t>(p) < segs.size()) ++childCount[p];
        }

        std::vector<float> packed;    // per-segment records (8 floats each)
        std::vector<float> instSeg;   // per-leaf segment index (into packed)
        packed.reserve(segs.size() * 8);
        float bmin[3] = { 1e30f, 1e30f, 1e30f };
        float bmax[3] = { -1e30f, -1e30f, -1e30f };

        for (size_t i = 0; i < segs.size(); ++i) {
            const auto& s = segs[i];
            bromath::Vec3 d = s.to - s.from;
            float len = bromath::vlen(d);
            if (len < 1e-6f) continue;
            if (s.depth < opts.minDepth) continue;
            if (s.radius > 0.0f && s.radius > opts.maxRadius) continue;
            if (opts.terminalOnly && childCount[i] > 0) continue;

            float weight = 1.0f;
            if (!opts.densityWeight.empty()) {
                weight = (i < opts.densityWeight.size())
                             ? std::max(0.0f, opts.densityWeight[i]) : 0.0f;
            }
            if (weight <= 0.0f) continue;

            // Stochastic round of the expected count, deterministic per segment
            // (splitmix-style hash for the fractional carry).
            float expected = len * opts.perUnitLength * weight;
            uint64_t h = opts.seed ^ (static_cast<uint64_t>(i) * 0x9E3779B97F4A7C15ULL);
            h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ULL; h ^= h >> 27;
            float frac = static_cast<float>((h >> 40) * (1.0 / 16777216.0));
            int count = static_cast<int>(std::floor(expected + frac));
            if (count <= 0) continue;
            if (count > 4096) count = 4096;   // sanity clamp

            // This packed segment's index, and one instSeg entry per leaf.
            float segIdx = static_cast<float>(packed.size() / 8);
            for (int k = 0; k < count; ++k) instSeg.push_back(segIdx);

            packed.push_back(s.from.x); packed.push_back(s.from.y);
            packed.push_back(s.from.z); packed.push_back(s.radius);
            packed.push_back(d.x); packed.push_back(d.y);
            packed.push_back(d.z); packed.push_back(0.0f);  // reserved

            float tox = s.to.x, toy = s.to.y, toz = s.to.z;
            bmin[0] = std::min({ bmin[0], s.from.x, tox });
            bmin[1] = std::min({ bmin[1], s.from.y, toy });
            bmin[2] = std::min({ bmin[2], s.from.z, toz });
            bmax[0] = std::max({ bmax[0], s.from.x, tox });
            bmax[1] = std::max({ bmax[1], s.from.y, toy });
            bmax[2] = std::max({ bmax[2], s.from.z, toz });
        }

        size_t segCount = packed.size() / 8;
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "segments",
                          makeFloat32Array(ctx, packed.empty() ? nullptr : packed.data(),
                                           packed.size()));
        JS_SetPropertyStr(ctx, obj, "instSeg",
                          makeFloat32Array(ctx, instSeg.empty() ? nullptr : instSeg.data(),
                                           instSeg.size()));
        JS_SetPropertyStr(ctx, obj, "segCount", JS_NewInt64(ctx, (int64_t)segCount));
        JS_SetPropertyStr(ctx, obj, "instanceCount", JS_NewInt64(ctx, (int64_t)instSeg.size()));
        if (segCount == 0) { bmin[0]=bmin[1]=bmin[2]=bmax[0]=bmax[1]=bmax[2]=0.0f; }
        JSValue bminA = JS_NewArray(ctx), bmaxA = JS_NewArray(ctx);
        for (int i = 0; i < 3; ++i) {
            JS_SetPropertyUint32(ctx, bminA, i, JS_NewFloat64(ctx, bmin[i]));
            JS_SetPropertyUint32(ctx, bmaxA, i, JS_NewFloat64(ctx, bmax[i]));
        }
        JS_SetPropertyStr(ctx, obj, "boundsMin", bminA);
        JS_SetPropertyStr(ctx, obj, "boundsMax", bmaxA);
        return obj;
    })

    // -- compact per-segment tube buffer for the GPU branch-tube node --
    .method("emitBranchTubes", [](FWW* w, JSContext* ctx, JSValueConst optsVal) -> JSValue {
        auto segs = broflora::emitWorldSegments(*w->world);

        float minRadius = 0.0f;
        if (JS_IsObject(optsVal))
            minRadius = (float)qjsbind::get_prop_number(ctx, optsVal, "minRadius", 0.0);

        std::vector<float> packed;   // per-segment records (8 floats each)
        packed.reserve(segs.size() * 8);
        float bmin[3] = { 1e30f, 1e30f, 1e30f };
        float bmax[3] = { -1e30f, -1e30f, -1e30f };
        float maxR = 0.0f;

        for (size_t i = 0; i < segs.size(); ++i) {
            const auto& s = segs[i];
            bromath::Vec3 d = s.to - s.from;
            if (bromath::vlen(d) < 1e-6f) continue;
            if (s.radius < minRadius) continue;

            // Pipe-model taper: base radius = parent's radius (thicker toward
            // the root), tip radius = this segment's own radius.
            float rTo = s.radius;
            float rFrom = rTo;
            int p = s.parent;
            if (p >= 0 && static_cast<size_t>(p) < segs.size() && segs[p].radius > 0.0f)
                rFrom = segs[p].radius;

            packed.push_back(s.from.x); packed.push_back(s.from.y);
            packed.push_back(s.from.z); packed.push_back(rFrom);
            packed.push_back(s.to.x);   packed.push_back(s.to.y);
            packed.push_back(s.to.z);   packed.push_back(rTo);

            maxR = std::max({ maxR, rFrom, rTo });
            bmin[0] = std::min({ bmin[0], s.from.x, s.to.x });
            bmin[1] = std::min({ bmin[1], s.from.y, s.to.y });
            bmin[2] = std::min({ bmin[2], s.from.z, s.to.z });
            bmax[0] = std::max({ bmax[0], s.from.x, s.to.x });
            bmax[1] = std::max({ bmax[1], s.from.y, s.to.y });
            bmax[2] = std::max({ bmax[2], s.from.z, s.to.z });
        }

        size_t segCount = packed.size() / 8;
        if (segCount == 0) {
            bmin[0]=bmin[1]=bmin[2]=bmax[0]=bmax[1]=bmax[2]=0.0f;
        } else {
            for (int i = 0; i < 3; ++i) { bmin[i] -= maxR; bmax[i] += maxR; }
        }

        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "segments",
                          makeFloat32Array(ctx, packed.empty() ? nullptr : packed.data(),
                                           packed.size()));
        JS_SetPropertyStr(ctx, obj, "segCount", JS_NewInt64(ctx, (int64_t)segCount));
        JSValue bminA = JS_NewArray(ctx), bmaxA = JS_NewArray(ctx);
        for (int i = 0; i < 3; ++i) {
            JS_SetPropertyUint32(ctx, bminA, i, JS_NewFloat64(ctx, bmin[i]));
            JS_SetPropertyUint32(ctx, bmaxA, i, JS_NewFloat64(ctx, bmax[i]));
        }
        JS_SetPropertyStr(ctx, obj, "boundsMin", bminA);
        JS_SetPropertyStr(ctx, obj, "boundsMax", bmaxA);
        return obj;
    })

    // -- fast native C++ branch segment transform buffer for InstancedMeshNode --
    .method("emitSegmentTransforms", [](FWW* w, JSContext* ctx) -> JSValue {
        auto segs = broflora::emitWorldSegments(*w->world);
        if (segs.empty()) {
            return makeFloat32Array(ctx, nullptr, 0);
        }
        size_t count = segs.size();
        std::vector<float> transforms(count * 16);

        #pragma omp parallel for schedule(static) if(count > 64)
        for (int i = 0; i < static_cast<int>(count); ++i) {
            const auto& seg = segs[static_cast<size_t>(i)];
            bromath::Vec3 d = seg.to - seg.from;
            float len = bromath::vlen(d);
            if (len < 1e-6f) len = 1e-6f;
            bromath::Vec3 fwd = d * (1.0f / len);

            bromath::Vec3 worldUp{0, 1, 0};
            bromath::Vec3 side = bromath::vcross(fwd, worldUp);
            if (bromath::vdot(side, side) < 1e-8f) {
                side = bromath::vcross(fwd, bromath::Vec3{1, 0, 0});
            }
            side = bromath::vnorm(side);
            bromath::Vec3 up = bromath::vnorm(bromath::vcross(side, fwd));

            float r = seg.radius > 0.001f ? seg.radius : 0.001f;
            bromath::Vec3 origin = seg.from;

            float* o = transforms.data() + static_cast<size_t>(i) * 16;
            o[0] = side.x * r;  o[1] = up.x * r;  o[2] = fwd.x * len;  o[3] = origin.x;
            o[4] = side.y * r;  o[5] = up.y * r;  o[6] = fwd.y * len;  o[7] = origin.y;
            o[8] = side.z * r;  o[9] = up.z * r;  o[10] = fwd.z * len; o[11] = origin.z;
            o[12] = 1.0f;       o[13] = 1.0f;      o[14] = 1.0f;       o[15] = 1.0f;
        }

        return makeFloat32Array(ctx, transforms.data(), count * 16);
    })

    .method("emitPlantFoliageMesh", [](FWW* w, JSContext* ctx, int plantIdx, JSValueConst leafVal, JSValueConst optsVal) -> JSValue {
        if (plantIdx < 0 || (size_t)plantIdx >= w->world->plants.size()) return JS_NULL;
        auto* lw = MeshBindings::getMeshData(ctx, leafVal);
        if (!lw || lw->empty()) return JS_ThrowTypeError(ctx, "emitPlantFoliageMesh requires valid leaf Mesh");

        const auto& plant = w->world->plants[(size_t)plantIdx];
        auto segs = broflora::emitPlantSegments(plant);
        if (segs.empty()) {
            return MeshBindings::wrapMeshData(ctx, std::make_unique<bromesh::MeshData>());
        }
        auto samples = broflora::emitPlantFoliage(plant);

        bromesh::LeafPlacementOptions opts;
        if (JS_IsObject(optsVal)) readLeafPlacementOptions(ctx, optsVal, opts);

        fillFoliageDensity(samples, segs.size(), opts);

        auto md = std::make_unique<bromesh::MeshData>(
            bromesh::scatterLeaves(segs, *lw, opts));
        return MeshBindings::wrapMeshData(ctx, std::move(md));
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

    // -- fast native C++ bloom mesh emitter --
    .method("emitBloomMesh", [](FWW* w, JSContext* ctx, JSValueConst petalVal, JSValueConst centerVal, JSValueConst optsVal) -> JSValue {
        auto* pw = MeshBindings::getMeshData(ctx, petalVal);
        auto* cw = MeshBindings::getMeshData(ctx, centerVal);
        if (!pw || pw->empty()) return JS_ThrowTypeError(ctx, "emitBloomMesh requires valid petal Mesh");

        auto anchors = broflora::emitWorldBloomAnchors(*w->world);
        if (anchors.empty()) {
            JSValue arr = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, arr, 0, MeshBindings::wrapMeshData(ctx, std::make_unique<bromesh::MeshData>()));
            JS_SetPropertyUint32(ctx, arr, 1, MeshBindings::wrapMeshData(ctx, std::make_unique<bromesh::MeshData>()));
            return arr;
        }

        uint32_t bloomCap = 500;
        float bloomLightMin = 0.18f;
        if (JS_IsObject(optsVal)) {
            readUint32Field(ctx, optsVal, "bloomCap", bloomCap);
            readFloatField(ctx, optsVal, "bloomLightMin", bloomLightMin);
        }
        if (bloomCap == 0) bloomCap = 1;

        size_t stride = (anchors.size() > bloomCap) ? (anchors.size() + bloomCap - 1) / bloomCap : 1;

        auto mergedPetals = std::make_unique<bromesh::MeshData>();
        auto mergedCenters = std::make_unique<bromesh::MeshData>();

        auto appendTransformed = [](bromesh::MeshData& target, const bromesh::MeshData& src,
                                    const bromath::Vec3& pos, const bromath::Vec3& norm, float scale) {
            if (src.empty()) return;
            uint32_t baseIndex = static_cast<uint32_t>(target.vertexCount());
            size_t nv = src.vertexCount();

            bromath::Vec3 n = norm;
            float len = std::hypot(n.x, std::hypot(n.y, n.z));
            if (len > 1e-6f) { n.x /= len; n.y /= len; n.z /= len; }
            else { n = {0.0f, 1.0f, 0.0f}; }

            float ny = std::max(-1.0f, std::min(1.0f, n.y));
            float ang = std::acos(ny);
            bromath::Vec3 axis{1.0f, 0.0f, 0.0f};
            if (ang >= 1e-4f) {
                if (ang > 3.14159265f - 1e-4f) {
                    axis = {1.0f, 0.0f, 0.0f};
                } else {
                    axis = {n.z, 0.0f, -n.x};
                    float al = std::hypot(axis.x, axis.z);
                    if (al > 1e-6f) { axis.x /= al; axis.z /= al; }
                    else { axis = {1.0f, 0.0f, 0.0f}; }
                }
            }

            float c = std::cos(ang), s = std::sin(ang);
            float omc = 1.0f - c;
            float R[3][3] = {
                { c + axis.x*axis.x*omc,          axis.x*axis.y*omc - axis.z*s, axis.x*axis.z*omc + axis.y*s },
                { axis.y*axis.x*omc + axis.z*s,  c + axis.y*axis.y*omc,          axis.y*axis.z*omc - axis.x*s },
                { axis.z*axis.x*omc - axis.y*s,  axis.z*axis.y*omc + axis.x*s,  c + axis.z*axis.z*omc          }
            };

            target.positions.reserve(target.positions.size() + nv * 3);
            target.normals.reserve(target.normals.size() + nv * 3);
            if (!src.uvs.empty()) target.uvs.reserve(target.uvs.size() + src.uvs.size());
            target.indices.reserve(target.indices.size() + src.indices.size());

            for (size_t i = 0; i < nv; ++i) {
                float px = src.positions[i * 3] * scale;
                float py = src.positions[i * 3 + 1] * scale;
                float pz = src.positions[i * 3 + 2] * scale;

                float rx = R[0][0]*px + R[0][1]*py + R[0][2]*pz + pos.x;
                float ry = R[1][0]*px + R[1][1]*py + R[1][2]*pz + pos.y;
                float rz = R[2][0]*px + R[2][1]*py + R[2][2]*pz + pos.z;

                target.positions.push_back(rx);
                target.positions.push_back(ry);
                target.positions.push_back(rz);

                if (src.hasNormals()) {
                    float nx = src.normals[i * 3];
                    float ny_ = src.normals[i * 3 + 1];
                    float nz = src.normals[i * 3 + 2];
                    float rnx = R[0][0]*nx + R[0][1]*ny_ + R[0][2]*nz;
                    float rny = R[1][0]*nx + R[1][1]*ny_ + R[1][2]*nz;
                    float rnz = R[2][0]*nx + R[2][1]*ny_ + R[2][2]*nz;
                    target.normals.push_back(rnx);
                    target.normals.push_back(rny);
                    target.normals.push_back(rnz);
                } else {
                    target.normals.push_back(0.0f);
                    target.normals.push_back(1.0f);
                    target.normals.push_back(0.0f);
                }

                if (src.hasUVs()) {
                    target.uvs.push_back(src.uvs[i * 2]);
                    target.uvs.push_back(src.uvs[i * 2 + 1]);
                }
            }

            for (size_t i = 0; i < src.indices.size(); ++i) {
                target.indices.push_back(baseIndex + src.indices[i]);
            }
        };

        for (size_t i = 0; i < anchors.size(); i += stride) {
            const auto& a = anchors[i];
            if (a.lightExposure01 < bloomLightMin) continue;
            float s = 0.8f + 0.5f * std::min(1.0f, a.age01);

            appendTransformed(*mergedPetals, *pw, a.position, a.normal, s);

            if (cw && !cw->empty()) {
                float lift = 0.012f * s;
                bromath::Vec3 cPos = {
                    a.position.x + a.normal.x * lift,
                    a.position.y + a.normal.y * lift,
                    a.position.z + a.normal.z * lift
                };
                appendTransformed(*mergedCenters, *cw, cPos, a.normal, s);
            }
        }

        JSValue arr = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, arr, 0, MeshBindings::wrapMeshData(ctx, std::move(mergedPetals)));
        JS_SetPropertyUint32(ctx, arr, 1, MeshBindings::wrapMeshData(ctx, std::move(mergedCenters)));
        return arr;
    })

    // -- per-plant emit (parallel to the world-level ones above) -----------
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
            JS_SetPropertyStr(ctx, o, "twigGrade01", JS_NewFloat64(ctx, s.twigGrade01));
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
    });
}

} // namespace bro::js

#endif // BRO_WITH_FLORA
