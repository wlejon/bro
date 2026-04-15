#include "js/rigging_bindings.h"
#include "js/mesh_bindings.h"

#include <qjsbind/qjsbind.h>

#include <bromesh/mesh_data.h>
#include <bromesh/manipulation/skin.h>
#include <bromesh/manipulation/skin_transfer.h>
#include <bromesh/rigging/skin_validate.h>
#include <bromesh/animation/pose.h>
#include <bromesh/animation/retarget.h>
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

struct SkeletonWrapper {
    std::unique_ptr<bromesh::Skeleton> skel;
};

struct PoseWrapper {
    std::unique_ptr<bromesh::Pose> pose;
};

struct AnimationWrapper {
    std::unique_ptr<bromesh::Animation> anim;
};

using SDW = SkinDataWrapper;
using VCW = VoxelChunkWrapper;
using SKW = SkeletonWrapper;
using PW  = PoseWrapper;
using AW  = AnimationWrapper;

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

static JSValue wrapSkeleton(JSContext* ctx, bromesh::Skeleton&& skel) {
    return qjsbind::wrap<SKW>(ctx,
        new SKW{std::make_unique<bromesh::Skeleton>(std::move(skel))});
}

static JSValue wrapPose(JSContext* ctx, bromesh::Pose&& pose) {
    return qjsbind::wrap<PW>(ctx,
        new PW{std::make_unique<bromesh::Pose>(std::move(pose))});
}

static JSValue wrapAnimation(JSContext* ctx, bromesh::Animation&& a) {
    return qjsbind::wrap<AW>(ctx,
        new AW{std::make_unique<bromesh::Animation>(std::move(a))});
}

// ---------------------------------------------------------------------------
// AnimChannel <-> JS object
// ---------------------------------------------------------------------------

static const char* pathToString(bromesh::AnimChannel::Path p) {
    switch (p) {
        case bromesh::AnimChannel::Path::Translation: return "translation";
        case bromesh::AnimChannel::Path::Rotation:    return "rotation";
        case bromesh::AnimChannel::Path::Scale:       return "scale";
    }
    return "translation";
}

static bromesh::AnimChannel::Path pathFromString(const char* s) {
    if (!s) return bromesh::AnimChannel::Path::Translation;
    if (std::strcmp(s, "rotation") == 0) return bromesh::AnimChannel::Path::Rotation;
    if (std::strcmp(s, "scale") == 0)    return bromesh::AnimChannel::Path::Scale;
    return bromesh::AnimChannel::Path::Translation;
}

static const char* interpToString(bromesh::AnimChannel::Interp i) {
    switch (i) {
        case bromesh::AnimChannel::Interp::Linear:      return "linear";
        case bromesh::AnimChannel::Interp::Step:        return "step";
        case bromesh::AnimChannel::Interp::CubicSpline: return "cubicSpline";
    }
    return "linear";
}

static bromesh::AnimChannel::Interp interpFromString(const char* s) {
    if (!s) return bromesh::AnimChannel::Interp::Linear;
    if (std::strcmp(s, "step") == 0)        return bromesh::AnimChannel::Interp::Step;
    if (std::strcmp(s, "cubicSpline") == 0) return bromesh::AnimChannel::Interp::CubicSpline;
    return bromesh::AnimChannel::Interp::Linear;
}

static bromesh::AnimChannel readChannel(JSContext* ctx, JSValueConst obj) {
    bromesh::AnimChannel c;
    JSValue v;

    v = JS_GetPropertyStr(ctx, obj, "boneIndex");
    if (!JS_IsUndefined(v)) { int32_t b = -1; JS_ToInt32(ctx, &b, v); c.boneIndex = b; }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, obj, "path");
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        c.path = pathFromString(s);
        if (s) JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, obj, "interp");
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        c.interp = interpFromString(s);
        if (s) JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, obj, "times");
    if (!JS_IsUndefined(v)) readFloatArrayVal(ctx, v, c.times);
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, obj, "values");
    if (!JS_IsUndefined(v)) readFloatArrayVal(ctx, v, c.values);
    JS_FreeValue(ctx, v);

    return c;
}

static JSValue makeChannel(JSContext* ctx, const bromesh::AnimChannel& c) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "boneIndex", JS_NewInt32(ctx, c.boneIndex));
    JS_SetPropertyStr(ctx, obj, "path",      JS_NewString(ctx, pathToString(c.path)));
    JS_SetPropertyStr(ctx, obj, "interp",    JS_NewString(ctx, interpToString(c.interp)));
    JS_SetPropertyStr(ctx, obj, "times",     makeFloat32Array(ctx, c.times));
    JS_SetPropertyStr(ctx, obj, "values",    makeFloat32Array(ctx, c.values));
    return obj;
}

static void readChannelsArray(JSContext* ctx, JSValueConst arr, std::vector<bromesh::AnimChannel>& out) {
    if (!JS_IsArray(arr)) return;
    JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0; JS_ToInt32(ctx, &len, lenVal); JS_FreeValue(ctx, lenVal);
    out.clear();
    out.reserve((size_t)len);
    for (int32_t i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        out.push_back(readChannel(ctx, e));
        JS_FreeValue(ctx, e);
    }
}

static JSValue makeChannelsArray(JSContext* ctx, const std::vector<bromesh::AnimChannel>& chs) {
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < chs.size(); i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, makeChannel(ctx, chs[i]));
    return arr;
}

static void readAnimationFromObj(JSContext* ctx, JSValueConst obj, bromesh::Animation& out) {
    JSValue v;

    v = JS_GetPropertyStr(ctx, obj, "name");
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (s) { out.name = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, obj, "duration");
    if (!JS_IsUndefined(v)) { double d = 0; JS_ToFloat64(ctx, &d, v); out.duration = (float)d; }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, obj, "channels");
    if (!JS_IsUndefined(v)) readChannelsArray(ctx, v, out.channels);
    JS_FreeValue(ctx, v);

    // If duration was omitted, derive it from max channel time.
    if (out.duration <= 0.0f) {
        for (const auto& c : out.channels)
            for (float t : c.times)
                if (t > out.duration) out.duration = t;
    }
}

// ---------------------------------------------------------------------------
// Bone / Socket JS object <-> struct conversion
// ---------------------------------------------------------------------------

static void readFloatN(JSContext* ctx, JSValueConst v, float* out, int n, const float* def) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        if (def) std::memcpy(out, def, sizeof(float) * (size_t)n);
        return;
    }
    if (JS_IsArray(v)) {
        for (int i = 0; i < n; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
            double tmp = def ? def[i] : 0.0;
            if (!JS_IsUndefined(e)) JS_ToFloat64(ctx, &tmp, e);
            out[i] = (float)tmp;
            JS_FreeValue(ctx, e);
        }
        return;
    }
    // Try TypedArray.
    std::vector<float> tmp;
    if (readFloatArrayVal(ctx, v, tmp)) {
        for (int i = 0; i < n; i++)
            out[i] = (i < (int)tmp.size()) ? tmp[i] : (def ? def[i] : 0.0f);
    } else if (def) {
        std::memcpy(out, def, sizeof(float) * (size_t)n);
    }
}

static JSValue makeFloatNArray(JSContext* ctx, const float* data, int n) {
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewFloat64(ctx, data[i]));
    return arr;
}

static bromesh::Bone readBone(JSContext* ctx, JSValueConst obj) {
    bromesh::Bone b;
    JSValue v;

    v = JS_GetPropertyStr(ctx, obj, "name");
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (s) { b.name = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, obj, "parent");
    if (!JS_IsUndefined(v)) { int32_t p = -1; JS_ToInt32(ctx, &p, v); b.parent = p; }
    JS_FreeValue(ctx, v);

    static const float defT[3] = {0,0,0};
    static const float defR[4] = {0,0,0,1};
    static const float defS[3] = {1,1,1};
    static const float defIB[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    v = JS_GetPropertyStr(ctx, obj, "localT");      readFloatN(ctx, v, b.localT,      3,  defT);  JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, obj, "localR");      readFloatN(ctx, v, b.localR,      4,  defR);  JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, obj, "localS");      readFloatN(ctx, v, b.localS,      3,  defS);  JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, obj, "inverseBind"); readFloatN(ctx, v, b.inverseBind, 16, defIB); JS_FreeValue(ctx, v);

    return b;
}

static JSValue makeBone(JSContext* ctx, const bromesh::Bone& b) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "name",        JS_NewString(ctx, b.name.c_str()));
    JS_SetPropertyStr(ctx, obj, "parent",      JS_NewInt32(ctx, b.parent));
    JS_SetPropertyStr(ctx, obj, "localT",      makeFloatNArray(ctx, b.localT, 3));
    JS_SetPropertyStr(ctx, obj, "localR",      makeFloatNArray(ctx, b.localR, 4));
    JS_SetPropertyStr(ctx, obj, "localS",      makeFloatNArray(ctx, b.localS, 3));
    JS_SetPropertyStr(ctx, obj, "inverseBind", makeFloatNArray(ctx, b.inverseBind, 16));
    return obj;
}

static bromesh::Socket readSocket(JSContext* ctx, JSValueConst obj) {
    bromesh::Socket s;
    JSValue v;

    v = JS_GetPropertyStr(ctx, obj, "name");
    if (JS_IsString(v)) {
        const char* str = JS_ToCString(ctx, v);
        if (str) { s.name = str; JS_FreeCString(ctx, str); }
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, obj, "bone");
    if (!JS_IsUndefined(v)) { int32_t b = 0; JS_ToInt32(ctx, &b, v); s.bone = b; }
    JS_FreeValue(ctx, v);

    static const float defM[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    v = JS_GetPropertyStr(ctx, obj, "offset"); readFloatN(ctx, v, s.offset, 16, defM); JS_FreeValue(ctx, v);

    return s;
}

static JSValue makeSocket(JSContext* ctx, const bromesh::Socket& s) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "name",   JS_NewString(ctx, s.name.c_str()));
    JS_SetPropertyStr(ctx, obj, "bone",   JS_NewInt32(ctx, s.bone));
    JS_SetPropertyStr(ctx, obj, "offset", makeFloatNArray(ctx, s.offset, 16));
    return obj;
}

static void readBonesArray(JSContext* ctx, JSValueConst arr, std::vector<bromesh::Bone>& out) {
    if (!JS_IsArray(arr)) return;
    JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0; JS_ToInt32(ctx, &len, lenVal); JS_FreeValue(ctx, lenVal);
    out.clear();
    out.reserve((size_t)len);
    for (int32_t i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        out.push_back(readBone(ctx, e));
        JS_FreeValue(ctx, e);
    }
}

static void readSocketsArray(JSContext* ctx, JSValueConst arr, std::vector<bromesh::Socket>& out) {
    if (!JS_IsArray(arr)) return;
    JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0; JS_ToInt32(ctx, &len, lenVal); JS_FreeValue(ctx, lenVal);
    out.clear();
    out.reserve((size_t)len);
    for (int32_t i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        out.push_back(readSocket(ctx, e));
        JS_FreeValue(ctx, e);
    }
}

static JSValue makeBonesArray(JSContext* ctx, const std::vector<bromesh::Bone>& bones) {
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < bones.size(); i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, makeBone(ctx, bones[i]));
    return arr;
}

static JSValue makeSocketsArray(JSContext* ctx, const std::vector<bromesh::Socket>& sks) {
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < sks.size(); i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, makeSocket(ctx, sks[i]));
    return arr;
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
    // Skeleton — bones + sockets + bind pose
    // =======================================================================
    qjsbind::Class<SKW>(ctx, "Skeleton")

    .constructor([](JSContext* ctx, int argc, JSValueConst* argv) -> SKW* {
        auto s = std::make_unique<bromesh::Skeleton>();
        if (argc > 0 && JS_IsObject(argv[0])) {
            JSValue v;
            v = JS_GetPropertyStr(ctx, argv[0], "bones");
            if (!JS_IsUndefined(v)) readBonesArray(ctx, v, s->bones);
            JS_FreeValue(ctx, v);
            v = JS_GetPropertyStr(ctx, argv[0], "sockets");
            if (!JS_IsUndefined(v)) readSocketsArray(ctx, v, s->sockets);
            JS_FreeValue(ctx, v);
        }
        return new SKW{std::move(s)};
    })

    .static_method("fromBones", [](JSContext* ctx, JSValue bones) -> JSValue {
        bromesh::Skeleton s;
        readBonesArray(ctx, bones, s.bones);
        return wrapSkeleton(ctx, std::move(s));
    })

    .prop("bones",
        [](SKW* w, JSContext* ctx) -> JSValue {
            return w->skel ? makeBonesArray(ctx, w->skel->bones) : JS_UNDEFINED;
        },
        [](SKW* w, JSContext* ctx, JSValue val) {
            if (w->skel) readBonesArray(ctx, val, w->skel->bones);
        })
    .prop("sockets",
        [](SKW* w, JSContext* ctx) -> JSValue {
            return w->skel ? makeSocketsArray(ctx, w->skel->sockets) : JS_UNDEFINED;
        },
        [](SKW* w, JSContext* ctx, JSValue val) {
            if (w->skel) readSocketsArray(ctx, val, w->skel->sockets);
        })

    .get("boneCount",   [](SKW* w) { return (int)(w->skel ? w->skel->bones.size()   : 0); })
    .get("socketCount", [](SKW* w) { return (int)(w->skel ? w->skel->sockets.size() : 0); })

    .method("findBone",   [](SKW* w, std::string name) -> int {
        return w->skel ? w->skel->findBone(name) : -1;
    })
    .method("findSocket", [](SKW* w, std::string name) -> int {
        return w->skel ? w->skel->findSocket(name) : -1;
    })

    .method("addSocket", [](SKW* w, JSContext* ctx, JSValue obj) -> int {
        if (!w->skel || !JS_IsObject(obj)) return -1;
        w->skel->sockets.push_back(readSocket(ctx, obj));
        return (int)w->skel->sockets.size() - 1;
    })

    .method("bindPose", [](SKW* w, JSContext* ctx) -> JSValue {
        if (!w->skel) return JS_UNDEFINED;
        return wrapPose(ctx, bromesh::bindPose(*w->skel));
    })

    .method("addRigifySockets", [](SKW* w) -> int {
        return w->skel ? bromesh::addRigifySockets(*w->skel) : 0;
    })

    .method("findBoneBySuffix", [](SKW* w, std::string suffix) -> int {
        return w->skel ? bromesh::findBoneBySuffix(*w->skel, suffix) : -1;
    })
    ;

    // =======================================================================
    // Pose — flat array of local TRS per bone (stride 10)
    // =======================================================================
    qjsbind::Class<PW>(ctx, "Pose")

    .constructor([](JSContext* ctx, int argc, JSValueConst* argv) -> PW* {
        auto p = std::make_unique<bromesh::Pose>();
        if (argc > 0) {
            // Either a Float32Array of pose data, or { data, boneCount }, or
            // a number = bone count (zero-init).
            if (JS_IsNumber(argv[0])) {
                int32_t bc = 0; JS_ToInt32(ctx, &bc, argv[0]);
                p->data.assign((size_t)bc * 10, 0.0f);
                // identity quaternion + unit scale per bone
                for (int32_t i = 0; i < bc; i++) {
                    p->data[i*10 + 6] = 1.0f; // qw
                    p->data[i*10 + 7] = 1.0f; // sx
                    p->data[i*10 + 8] = 1.0f; // sy
                    p->data[i*10 + 9] = 1.0f; // sz
                }
            } else if (JS_IsObject(argv[0])) {
                JSValue v = JS_GetPropertyStr(ctx, argv[0], "data");
                if (!JS_IsUndefined(v)) readFloatArrayVal(ctx, v, p->data);
                else readFloatArrayVal(ctx, argv[0], p->data);
                JS_FreeValue(ctx, v);
            }
        }
        return new PW{std::move(p)};
    })

    .prop("data",
        [](PW* w, JSContext* ctx) -> JSValue {
            return w->pose ? makeFloat32Array(ctx, w->pose->data) : JS_UNDEFINED;
        },
        [](PW* w, JSContext* ctx, JSValue val) {
            if (w->pose) readFloatArrayVal(ctx, val, w->pose->data);
        })

    .get("boneCount", [](PW* w) {
        return (int)(w->pose ? w->pose->boneCount() : 0);
    })

    .method("clone", [](PW* w, JSContext* ctx) -> JSValue {
        if (!w->pose) return JS_UNDEFINED;
        return wrapPose(ctx, bromesh::Pose(*w->pose));
    })

    .method("computeWorldMatrices", [](PW* w, JSContext* ctx, JSValue skelVal) -> JSValue {
        if (!w->pose) return JS_UNDEFINED;
        auto* sk = qjsbind::unwrap<SKW>(ctx, skelVal);
        if (!sk || !sk->skel) return JS_ThrowTypeError(ctx, "argument must be a Skeleton");
        std::vector<float> out;
        bromesh::computeWorldMatrices(*sk->skel, *w->pose, out);
        return makeFloat32Array(ctx, out);
    })

    .method("computeSkinningMatrices", [](PW* w, JSContext* ctx, JSValue skelVal) -> JSValue {
        if (!w->pose) return JS_UNDEFINED;
        auto* sk = qjsbind::unwrap<SKW>(ctx, skelVal);
        if (!sk || !sk->skel) return JS_ThrowTypeError(ctx, "argument must be a Skeleton");
        std::vector<float> out;
        bromesh::computeSkinningMatrices(*sk->skel, *w->pose, out);
        return makeFloat32Array(ctx, out);
    })

    .method("socketWorld", [](PW* w, JSContext* ctx, JSValue skelVal, std::string name) -> JSValue {
        if (!w->pose) return JS_NULL;
        auto* sk = qjsbind::unwrap<SKW>(ctx, skelVal);
        if (!sk || !sk->skel) return JS_ThrowTypeError(ctx, "first argument must be a Skeleton");
        float m[16];
        if (!bromesh::socketWorldMatrix(*sk->skel, *w->pose, name, m)) return JS_NULL;
        std::vector<float> v(m, m + 16);
        return makeFloat32Array(ctx, v);
    })

    .static_method("blend", [](JSContext* ctx, JSValue aVal, JSValue bVal, double weight,
                                std::optional<JSValue> mask) -> JSValue {
        auto* aw = qjsbind::unwrap<PW>(ctx, aVal);
        auto* bw = qjsbind::unwrap<PW>(ctx, bVal);
        if (!aw || !aw->pose || !bw || !bw->pose)
            return JS_ThrowTypeError(ctx, "blend requires two Pose arguments");
        std::vector<uint8_t> maskVec;
        const uint8_t* maskPtr = nullptr;
        if (mask && !JS_IsUndefined(*mask) && !JS_IsNull(*mask)) {
            if (readUint8ArrayVal(ctx, *mask, maskVec) && !maskVec.empty())
                maskPtr = maskVec.data();
        }
        bromesh::blendPoses(*aw->pose, *bw->pose, (float)weight, maskPtr);
        return JS_DupValue(ctx, aVal);
    })
    ;

    // =======================================================================
    // Animation — keyframed channels, evaluated to Pose
    // =======================================================================
    qjsbind::Class<AW>(ctx, "Animation")

    .constructor([](JSContext* ctx, int argc, JSValueConst* argv) -> AW* {
        auto a = std::make_unique<bromesh::Animation>();
        if (argc > 0 && JS_IsObject(argv[0])) readAnimationFromObj(ctx, argv[0], *a);
        return new AW{std::move(a)};
    })

    .prop("name",
        [](AW* w, JSContext* ctx) -> JSValue {
            return w->anim ? JS_NewString(ctx, w->anim->name.c_str()) : JS_UNDEFINED;
        },
        [](AW* w, JSContext* ctx, JSValue val) {
            if (!w->anim) return;
            const char* s = JS_ToCString(ctx, val);
            if (s) { w->anim->name = s; JS_FreeCString(ctx, s); }
        })

    .prop("duration",
        [](AW* w) { return (double)(w->anim ? w->anim->duration : 0.0f); },
        [](AW* w, double d) { if (w->anim) w->anim->duration = (float)d; })

    .prop("channels",
        [](AW* w, JSContext* ctx) -> JSValue {
            return w->anim ? makeChannelsArray(ctx, w->anim->channels) : JS_UNDEFINED;
        },
        [](AW* w, JSContext* ctx, JSValue val) {
            if (w->anim) readChannelsArray(ctx, val, w->anim->channels);
        })

    .get("channelCount", [](AW* w) { return (int)(w->anim ? w->anim->channels.size() : 0); })

    .method("evaluate", [](AW* w, JSContext* ctx, JSValue skelVal, double t,
                            std::optional<JSValue> opts) -> JSValue {
        if (!w->anim) return JS_UNDEFINED;
        auto* sk = qjsbind::unwrap<SKW>(ctx, skelVal);
        if (!sk || !sk->skel) return JS_ThrowTypeError(ctx, "first argument must be a Skeleton");
        bool loop = true;
        if (opts && JS_IsObject(*opts)) {
            JSValue v = JS_GetPropertyStr(ctx, *opts, "loop");
            if (!JS_IsUndefined(v)) loop = JS_ToBool(ctx, v);
            JS_FreeValue(ctx, v);
        }
        return wrapPose(ctx, bromesh::evaluateAnimation(*sk->skel, *w->anim, (float)t, loop));
    })

    .method("evaluateInto", [](AW* w, JSContext* ctx, JSValue skelVal, double t,
                                JSValue poseVal, std::optional<JSValue> opts) -> JSValue {
        if (!w->anim) return JS_UNDEFINED;
        auto* sk = qjsbind::unwrap<SKW>(ctx, skelVal);
        auto* pw = qjsbind::unwrap<PW>(ctx, poseVal);
        if (!sk || !sk->skel || !pw || !pw->pose)
            return JS_ThrowTypeError(ctx, "evaluateInto requires (Skeleton, t, Pose)");
        bool loop = true;
        if (opts && JS_IsObject(*opts)) {
            JSValue v = JS_GetPropertyStr(ctx, *opts, "loop");
            if (!JS_IsUndefined(v)) loop = JS_ToBool(ctx, v);
            JS_FreeValue(ctx, v);
        }
        bromesh::evaluateAnimationInto(*sk->skel, *w->anim, (float)t, loop, *pw->pose);
        return JS_DupValue(ctx, poseVal);
    })

    .static_method("retarget", [](JSContext* ctx, JSValue animVal, JSValue srcSkel, JSValue dstSkel) -> JSValue {
        auto* aw = qjsbind::unwrap<AW>(ctx, animVal);
        auto* ss = qjsbind::unwrap<SKW>(ctx, srcSkel);
        auto* ds = qjsbind::unwrap<SKW>(ctx, dstSkel);
        if (!aw || !aw->anim || !ss || !ss->skel || !ds || !ds->skel)
            return JS_ThrowTypeError(ctx, "retarget requires (Animation, srcSkeleton, dstSkeleton)");
        return wrapAnimation(ctx, bromesh::retargetAnimation(*aw->anim, *ss->skel, *ds->skel));
    })
    ;

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

bromesh::Skeleton* RiggingBindings::getSkeleton(JSContext* ctx, JSValueConst val) {
    auto* w = qjsbind::unwrap<SKW>(ctx, val);
    return w ? w->skel.get() : nullptr;
}

JSValue RiggingBindings::wrapSkeleton(JSContext* ctx, bromesh::Skeleton&& skel) {
    return bro::js::wrapSkeleton(ctx, std::move(skel));
}

bromesh::Pose* RiggingBindings::getPose(JSContext* ctx, JSValueConst val) {
    auto* w = qjsbind::unwrap<PW>(ctx, val);
    return w ? w->pose.get() : nullptr;
}

JSValue RiggingBindings::wrapPose(JSContext* ctx, bromesh::Pose&& pose) {
    return bro::js::wrapPose(ctx, std::move(pose));
}

} // namespace bro::js
