#include "js/rigging_bindings.h"
#include "js/mesh_bindings.h"

#include <qjsbind/qjsbind.h>

#include <bromesh/mesh_data.h>
#include <bromesh/manipulation/skin.h>
#include <bromesh/manipulation/skin_transfer.h>
#include <bromesh/rigging/skin_validate.h>
#include <bromesh/voxel/voxel_chunk.h>

#include <cstring>
#include <memory>
#include <string>

namespace bro::js {

// ---------------------------------------------------------------------------
// Wrappers
// ---------------------------------------------------------------------------

struct SkinDataWrapper {
    std::unique_ptr<bromesh::SkinData> data;
};

struct VoxelChunkWrapper {
    std::unique_ptr<bromesh::VoxelChunk> chunk;
};

using SDW = SkinDataWrapper;
using VCW = VoxelChunkWrapper;

// ---------------------------------------------------------------------------
// TypedArray helpers (local copies — keep this file standalone from
// mesh_bindings.cpp's static helpers).
// ---------------------------------------------------------------------------

static bool readFloatArrayVal(JSContext* ctx, JSValueConst v, std::vector<float>& out) {
    size_t offset = 0, byteLen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, v, &offset, &byteLen, &bpe);
    if (JS_IsException(abuf)) { JS_FreeValue(ctx, abuf); return false; }
    size_t abufLen = 0;
    uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    if (!raw) return false;
    const float* data = reinterpret_cast<const float*>(raw + offset);
    out.assign(data, data + byteLen / sizeof(float));
    return true;
}

static bool readUint32ArrayVal(JSContext* ctx, JSValueConst v, std::vector<uint32_t>& out) {
    size_t offset = 0, byteLen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, v, &offset, &byteLen, &bpe);
    if (JS_IsException(abuf)) { JS_FreeValue(ctx, abuf); return false; }
    size_t abufLen = 0;
    uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    if (!raw) return false;
    const uint32_t* data = reinterpret_cast<const uint32_t*>(raw + offset);
    out.assign(data, data + byteLen / sizeof(uint32_t));
    return true;
}

static bool readUint8ArrayVal(JSContext* ctx, JSValueConst v, std::vector<uint8_t>& out) {
    size_t offset = 0, byteLen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, v, &offset, &byteLen, &bpe);
    if (JS_IsException(abuf)) { JS_FreeValue(ctx, abuf); return false; }
    size_t abufLen = 0;
    uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    if (!raw) return false;
    out.assign(raw + offset, raw + offset + byteLen);
    return true;
}

static JSValue makeFloat32Array(JSContext* ctx, const std::vector<float>& vec) {
    size_t bytes = vec.size() * sizeof(float);
    JSValue abuf = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(vec.data()), bytes);
    JSValue args[3] = { abuf, JS_UNDEFINED, JS_UNDEFINED };
    JSValue arr = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(ctx, abuf);
    return arr;
}

static JSValue makeUint32Array(JSContext* ctx, const std::vector<uint32_t>& vec) {
    size_t bytes = vec.size() * sizeof(uint32_t);
    JSValue abuf = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(vec.data()), bytes);
    JSValue args[3] = { abuf, JS_UNDEFINED, JS_UNDEFINED };
    JSValue arr = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_UINT32);
    JS_FreeValue(ctx, abuf);
    return arr;
}

static JSValue makeUint8Array(JSContext* ctx, const uint8_t* data, size_t len) {
    JSValue abuf = JS_NewArrayBufferCopy(ctx, data, len);
    JSValue args[3] = { abuf, JS_UNDEFINED, JS_UNDEFINED };
    JSValue arr = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, abuf);
    return arr;
}

// ---------------------------------------------------------------------------
// Wrap helpers
// ---------------------------------------------------------------------------

static JSValue wrapSkinData(JSContext* ctx, bromesh::SkinData&& data) {
    return qjsbind::wrap<SDW>(ctx,
        new SDW{std::make_unique<bromesh::SkinData>(std::move(data))});
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void RiggingBindings::install(JSContext* ctx) {

    // =======================================================================
    // SkinData — per-vertex bone weights/indices + inverse bind matrices
    // =======================================================================
    qjsbind::Class<SDW>(ctx, "SkinData")

    .constructor([](JSContext* ctx, int argc, JSValueConst* argv) -> SDW* {
        auto sd = std::make_unique<bromesh::SkinData>();
        if (argc > 0 && JS_IsObject(argv[0])) {
            JSValue v;

            v = JS_GetPropertyStr(ctx, argv[0], "boneWeights");
            if (!JS_IsUndefined(v)) readFloatArrayVal(ctx, v, sd->boneWeights);
            JS_FreeValue(ctx, v);

            v = JS_GetPropertyStr(ctx, argv[0], "boneIndices");
            if (!JS_IsUndefined(v)) readUint32ArrayVal(ctx, v, sd->boneIndices);
            JS_FreeValue(ctx, v);

            v = JS_GetPropertyStr(ctx, argv[0], "inverseBindMatrices");
            if (!JS_IsUndefined(v)) readFloatArrayVal(ctx, v, sd->inverseBindMatrices);
            JS_FreeValue(ctx, v);

            v = JS_GetPropertyStr(ctx, argv[0], "boneCount");
            if (!JS_IsUndefined(v)) {
                int32_t bc = 0; JS_ToInt32(ctx, &bc, v);
                sd->boneCount = (size_t)bc;
            }
            JS_FreeValue(ctx, v);

            // Default boneCount from inverseBindMatrices size if not set.
            if (sd->boneCount == 0 && !sd->inverseBindMatrices.empty())
                sd->boneCount = sd->inverseBindMatrices.size() / 16;
        }
        return new SDW{std::move(sd)};
    })

    // ── TypedArray properties (getter + setter) ─────────────────────────
    .prop("boneWeights",
        [](SDW* w, JSContext* ctx) -> JSValue {
            return w->data ? makeFloat32Array(ctx, w->data->boneWeights) : JS_UNDEFINED;
        },
        [](SDW* w, JSContext* ctx, JSValue val) {
            if (w->data) readFloatArrayVal(ctx, val, w->data->boneWeights);
        })
    .prop("boneIndices",
        [](SDW* w, JSContext* ctx) -> JSValue {
            return w->data ? makeUint32Array(ctx, w->data->boneIndices) : JS_UNDEFINED;
        },
        [](SDW* w, JSContext* ctx, JSValue val) {
            if (w->data) readUint32ArrayVal(ctx, val, w->data->boneIndices);
        })
    .prop("inverseBindMatrices",
        [](SDW* w, JSContext* ctx) -> JSValue {
            return w->data ? makeFloat32Array(ctx, w->data->inverseBindMatrices) : JS_UNDEFINED;
        },
        [](SDW* w, JSContext* ctx, JSValue val) {
            if (w->data) readFloatArrayVal(ctx, val, w->data->inverseBindMatrices);
        })
    .prop("boneCount",
        [](SDW* w) { return (int)(w->data ? w->data->boneCount : 0); },
        [](SDW* w, int n) { if (w->data) w->data->boneCount = (size_t)(n < 0 ? 0 : n); })

    // Vertex count derivable from boneWeights / 4
    .get("vertexCount", [](SDW* w) {
        return (int)(w->data ? w->data->boneWeights.size() / 4 : 0);
    })

    // ── Methods ─────────────────────────────────────────────────────────
    .method("clone", [](SDW* w, JSContext* ctx) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        return wrapSkinData(ctx, bromesh::SkinData(*w->data));
    })

    .method("normalize", [](SDW* w) {
        if (w->data) bromesh::normalizeWeights(*w->data);
    }, qjsbind::returns_this)

    // ── Static methods ──────────────────────────────────────────────────
    .static_method("validate", [](JSContext* ctx, JSValue meshVal, JSValue skinVal,
                                  std::optional<JSValue> options) -> JSValue {
        auto* mesh = MeshBindings::getMeshData(ctx, meshVal);
        auto* sw   = qjsbind::unwrap<SDW>(ctx, skinVal);
        if (!mesh || !sw || !sw->data)
            return JS_ThrowTypeError(ctx, "validate requires (Mesh, SkinData)");

        int influences = 4;
        float sumTol = 1e-3f;
        if (options && JS_IsObject(*options)) {
            JSValue v;
            v = JS_GetPropertyStr(ctx, *options, "influences");
            if (!JS_IsUndefined(v)) { int32_t x; JS_ToInt32(ctx, &x, v); influences = x; }
            JS_FreeValue(ctx, v);
            v = JS_GetPropertyStr(ctx, *options, "sumTolerance");
            if (!JS_IsUndefined(v)) { double x; JS_ToFloat64(ctx, &x, v); sumTol = (float)x; }
            JS_FreeValue(ctx, v);
        }

        auto v = bromesh::validateSkin(*mesh, *sw->data, influences, sumTol);
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "vertexCount",            JS_NewInt64(ctx, (int64_t)v.vertexCount));
        JS_SetPropertyStr(ctx, obj, "orphanCount",            JS_NewInt64(ctx, (int64_t)v.orphanCount));
        JS_SetPropertyStr(ctx, obj, "badSumCount",            JS_NewInt64(ctx, (int64_t)v.badSumCount));
        JS_SetPropertyStr(ctx, obj, "nanCount",               JS_NewInt64(ctx, (int64_t)v.nanCount));
        JS_SetPropertyStr(ctx, obj, "maxSumDeviation",        JS_NewFloat64(ctx, v.maxSumDeviation));
        JS_SetPropertyStr(ctx, obj, "maxInfluencesObserved",  JS_NewInt32(ctx, v.maxInfluencesObserved));
        JS_SetPropertyStr(ctx, obj, "clean",                  JS_NewBool(ctx, v.clean()));
        return obj;
    })

    .static_method("transfer", [](JSContext* ctx, JSValue targetMesh, JSValue sourceMesh,
                                  JSValue sourceSkin, std::optional<double> maxDistance) -> JSValue {
        auto* tgt = MeshBindings::getMeshData(ctx, targetMesh);
        auto* src = MeshBindings::getMeshData(ctx, sourceMesh);
        auto* sw  = qjsbind::unwrap<SDW>(ctx, sourceSkin);
        if (!tgt || !src || !sw || !sw->data)
            return JS_ThrowTypeError(ctx, "transfer requires (targetMesh, sourceMesh, sourceSkin)");
        auto out = bromesh::transferSkinWeights(*tgt, *src, *sw->data,
                                                (float)maxDistance.value_or(0.0));
        return wrapSkinData(ctx, std::move(out));
    })
    ;

    // =======================================================================
    // VoxelChunk — fixed-height voxel grid with greedy meshing
    // =======================================================================
    qjsbind::Class<VCW>(ctx, "VoxelChunk")

    .constructor([](JSContext* ctx, int argc, JSValueConst* argv) -> VCW* {
        int32_t sx = 0, sy = 0, sz = 0;
        double cs = 1.0;
        if (argc > 0) JS_ToInt32(ctx, &sx, argv[0]);
        if (argc > 1) JS_ToInt32(ctx, &sy, argv[1]);
        if (argc > 2) JS_ToInt32(ctx, &sz, argv[2]);
        if (argc > 3) JS_ToFloat64(ctx, &cs, argv[3]);
        if (sx <= 0 || sy <= 0 || sz <= 0) {
            // Construct a 1x1x1 placeholder rather than crashing in the ctor.
            sx = sy = sz = 1;
        }
        return new VCW{std::make_unique<bromesh::VoxelChunk>(sx, sy, sz, (float)cs)};
    })

    .get("sizeX",    [](VCW* w) { return w->chunk ? w->chunk->sizeX() : 0; })
    .get("sizeY",    [](VCW* w) { return w->chunk ? w->chunk->sizeY() : 0; })
    .get("sizeZ",    [](VCW* w) { return w->chunk ? w->chunk->sizeZ() : 0; })
    .get("cellSize", [](VCW* w) { return (double)(w->chunk ? w->chunk->cellSize() : 0.0f); })

    .prop("isDirty",
        [](VCW* w) { return w->chunk && w->chunk->isDirty(); },
        [](VCW* w, bool v) {
            if (!w->chunk) return;
            if (v) w->chunk->markDirty(); else w->chunk->clearDirty();
        })

    .method("getVoxel", [](VCW* w, int x, int y, int z) -> int {
        return w->chunk ? (int)w->chunk->getVoxel(x, y, z) : 0;
    })

    .method("setVoxel", [](VCW* w, int x, int y, int z, int material) {
        if (w->chunk) w->chunk->setVoxel(x, y, z, (uint8_t)material);
    }, qjsbind::returns_this)

    .method("fill", [](VCW* w, int value) {
        if (w->chunk) w->chunk->fill((uint8_t)value);
    }, qjsbind::returns_this)

    .method("markDirty",  [](VCW* w) { if (w->chunk) w->chunk->markDirty(); }, qjsbind::returns_this)
    .method("clearDirty", [](VCW* w) { if (w->chunk) w->chunk->clearDirty(); }, qjsbind::returns_this)

    .method("data", [](VCW* w, JSContext* ctx) -> JSValue {
        if (!w->chunk) return JS_UNDEFINED;
        size_t n = (size_t)w->chunk->sizeX() * (size_t)w->chunk->sizeY() * (size_t)w->chunk->sizeZ();
        return makeUint8Array(ctx, w->chunk->data(), n);
    })

    .method("setData", [](VCW* w, JSContext* ctx, JSValue val) {
        if (!w->chunk) return;
        std::vector<uint8_t> bytes;
        if (!readUint8ArrayVal(ctx, val, bytes)) return;
        size_t n = (size_t)w->chunk->sizeX() * (size_t)w->chunk->sizeY() * (size_t)w->chunk->sizeZ();
        size_t copyN = bytes.size() < n ? bytes.size() : n;
        std::memcpy(w->chunk->data(), bytes.data(), copyN);
        w->chunk->markDirty();
    }, qjsbind::returns_this)

    .method("buildMesh", [](VCW* w, JSContext* ctx, std::optional<JSValue> palette,
                             std::optional<int> paletteCount) -> JSValue {
        if (!w->chunk) return JS_UNDEFINED;
        std::vector<float> pal;
        const float* palPtr = nullptr;
        int palCount = 0;
        if (palette && !JS_IsUndefined(*palette) && !JS_IsNull(*palette)) {
            if (!readFloatArrayVal(ctx, *palette, pal))
                return JS_ThrowTypeError(ctx, "palette must be Float32Array");
            palPtr = pal.data();
            palCount = paletteCount ? *paletteCount : (int)(pal.size() / 4);
        }
        auto mesh = w->chunk->buildMesh(palPtr, palCount);
        return MeshBindings::wrapMeshData(ctx,
            std::make_unique<bromesh::MeshData>(std::move(mesh)));
    })
    ;
}

void RiggingBindings::cleanup(JSContext*) {}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bromesh::SkinData* RiggingBindings::getSkinData(JSContext* ctx, JSValueConst val) {
    auto* w = qjsbind::unwrap<SDW>(ctx, val);
    return w ? w->data.get() : nullptr;
}

JSValue RiggingBindings::wrapSkinData(JSContext* ctx, bromesh::SkinData&& data) {
    return bro::js::wrapSkinData(ctx, std::move(data));
}

} // namespace bro::js
