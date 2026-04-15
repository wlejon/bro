#include "js/mesh_bindings.h"
#include "js/rigging_bindings.h"

#include <qjsbind/qjsbind.h>

#include <bromesh/mesh_data.h>
#include <bromesh/primitives/primitives.h>
#include <bromesh/primitives/par_primitives.h>
#include <bromesh/analysis/bbox.h>
#include <bromesh/analysis/raycast.h>
#include <bromesh/analysis/intersect.h>
#include <bromesh/analysis/bvh.h>
#include <bromesh/analysis/sample.h>
#include <bromesh/analysis/bake.h>
#include <bromesh/analysis/bake_texture.h>
#include <bromesh/analysis/bake_transfer.h>
#include <bromesh/analysis/convex_decomposition.h>
#include <bromesh/manipulation/normals.h>
#include <bromesh/manipulation/simplify.h>
#include <bromesh/manipulation/subdivide.h>
#include <bromesh/manipulation/weld.h>
#include <bromesh/manipulation/smooth.h>
#include <bromesh/manipulation/remesh.h>
#include <bromesh/manipulation/repair.h>
#include <bromesh/manipulation/split_components.h>
#include <bromesh/manipulation/shrinkwrap.h>
#include <bromesh/manipulation/skin.h>
#include <bromesh/optimization/optimize.h>
#include <bromesh/optimization/analyze.h>
#include <bromesh/optimization/meshlets.h>
#include <bromesh/optimization/strips.h>
#include <bromesh/optimization/encode.h>
#include <bromesh/optimization/progressive.h>
#include <bromesh/optimization/spatial.h>
#include <bromesh/isosurface/marching_cubes.h>
#include <bromesh/isosurface/dual_contouring.h>
#include <bromesh/isosurface/surface_nets.h>
#include <bromesh/isosurface/transvoxel.h>
#include <bromesh/csg/boolean.h>
#include <bromesh/uv/unwrap.h>
#include <bromesh/uv/projection.h>
#include <bromesh/uv/uv_metrics.h>
#include <bromesh/voxel/greedy_mesh.h>
#include <bromesh/voxel/voxel_chunk.h>
#include <bromesh/io/gltf.h>
#include <bromesh/io/obj.h>
#include <bromesh/io/fbx.h>
#include <bromesh/io/ply.h>
#include <bromesh/io/stl.h>
#include <bromesh/io/vox.h>
#include <bromesh/reconstruction/reconstruct.h>

#include <cstring>
#include <string>
#include <memory>
#include <cmath>

namespace bro::js {

// ---------------------------------------------------------------------------
// Opaque wrapper
// ---------------------------------------------------------------------------

struct MeshWrapper {
    std::unique_ptr<bromesh::MeshData> data;
};

struct BVHWrapper {
    std::unique_ptr<bromesh::MeshBVH> bvh;
};

struct ProgressiveMeshWrapper {
    std::unique_ptr<bromesh::ProgressiveMesh> pm;
};

using MW  = MeshWrapper;
using BVW = BVHWrapper;
using PMW = ProgressiveMeshWrapper;

// ---------------------------------------------------------------------------
// Helpers — TypedArray I/O (domain-specific, not covered by qjsbind)
// ---------------------------------------------------------------------------

static bool readFloatArray(JSContext* ctx, JSValueConst obj, const char* prop,
                           std::vector<float>& out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return false; }

    size_t offset = 0, byteLen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, v, &offset, &byteLen, &bpe);
    if (JS_IsException(abuf)) {
        JS_FreeValue(ctx, abuf);
        JS_FreeValue(ctx, v);
        return false;
    }
    size_t abufLen = 0;
    uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    if (!raw) { JS_FreeValue(ctx, v); return false; }

    const float* data = reinterpret_cast<const float*>(raw + offset);
    out.assign(data, data + byteLen / sizeof(float));
    JS_FreeValue(ctx, v);
    return true;
}

static bool readUint32Array(JSContext* ctx, JSValueConst obj, const char* prop,
                            std::vector<uint32_t>& out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return false; }

    size_t offset = 0, byteLen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, v, &offset, &byteLen, &bpe);
    if (JS_IsException(abuf)) {
        JS_FreeValue(ctx, abuf);
        JS_FreeValue(ctx, v);
        return false;
    }
    size_t abufLen = 0;
    uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    if (!raw) { JS_FreeValue(ctx, v); return false; }

    const uint32_t* data = reinterpret_cast<const uint32_t*>(raw + offset);
    out.assign(data, data + byteLen / sizeof(uint32_t));
    JS_FreeValue(ctx, v);
    return true;
}

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

static JSValue makeUint8Array(JSContext* ctx, const std::vector<uint8_t>& vec) {
    JSValue abuf = JS_NewArrayBufferCopy(ctx, vec.data(), vec.size());
    JSValue args[3] = { abuf, JS_UNDEFINED, JS_UNDEFINED };
    JSValue arr = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, abuf);
    return arr;
}

static JSValue makeTextureBuffer(JSContext* ctx, const bromesh::TextureBuffer& tb) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width",    JS_NewInt32(ctx, tb.width));
    JS_SetPropertyStr(ctx, obj, "height",   JS_NewInt32(ctx, tb.height));
    JS_SetPropertyStr(ctx, obj, "channels", JS_NewInt32(ctx, tb.channels));
    JS_SetPropertyStr(ctx, obj, "pixels",   makeFloat32Array(ctx, tb.pixels));
    return obj;
}

static void readVec3(JSContext* ctx, JSValueConst arr, float out[3]) {
    double tmp;
    for (int i = 0; i < 3; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        JS_ToFloat64(ctx, &tmp, e);
        out[i] = (float)tmp;
        JS_FreeValue(ctx, e);
    }
}

// ---------------------------------------------------------------------------
// Helpers — RayHit and BBox JS object creation
// ---------------------------------------------------------------------------

static JSValue makeRayHit(JSContext* ctx, const bromesh::RayHit& h) {
    if (!h.hit) return JS_NULL;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "distance", JS_NewFloat64(ctx, h.distance));
    JSValue pos = JS_NewArray(ctx);
    JSValue nrm = JS_NewArray(ctx);
    for (int i = 0; i < 3; i++) {
        JS_SetPropertyUint32(ctx, pos, i, JS_NewFloat64(ctx, h.position[i]));
        JS_SetPropertyUint32(ctx, nrm, i, JS_NewFloat64(ctx, h.normal[i]));
    }
    JS_SetPropertyStr(ctx, obj, "position", pos);
    JS_SetPropertyStr(ctx, obj, "normal", nrm);
    JS_SetPropertyStr(ctx, obj, "triangleIndex", JS_NewInt32(ctx, (int32_t)h.triangleIndex));
    JS_SetPropertyStr(ctx, obj, "baryU", JS_NewFloat64(ctx, h.baryU));
    JS_SetPropertyStr(ctx, obj, "baryV", JS_NewFloat64(ctx, h.baryV));
    JS_SetPropertyStr(ctx, obj, "baryW", JS_NewFloat64(ctx, h.baryW));
    return obj;
}

static JSValue makeBBox(JSContext* ctx, const bromesh::BBox& bb) {
    JSValue obj = JS_NewObject(ctx);
    JSValue minArr = JS_NewArray(ctx);
    JSValue maxArr = JS_NewArray(ctx);
    for (int i = 0; i < 3; i++) {
        JS_SetPropertyUint32(ctx, minArr, i, JS_NewFloat64(ctx, bb.min[i]));
        JS_SetPropertyUint32(ctx, maxArr, i, JS_NewFloat64(ctx, bb.max[i]));
    }
    JS_SetPropertyStr(ctx, obj, "min", minArr);
    JS_SetPropertyStr(ctx, obj, "max", maxArr);
    JS_SetPropertyStr(ctx, obj, "centerX", JS_NewFloat64(ctx, bb.centerX()));
    JS_SetPropertyStr(ctx, obj, "centerY", JS_NewFloat64(ctx, bb.centerY()));
    JS_SetPropertyStr(ctx, obj, "centerZ", JS_NewFloat64(ctx, bb.centerZ()));
    JS_SetPropertyStr(ctx, obj, "extentX", JS_NewFloat64(ctx, bb.extentX()));
    JS_SetPropertyStr(ctx, obj, "extentY", JS_NewFloat64(ctx, bb.extentY()));
    JS_SetPropertyStr(ctx, obj, "extentZ", JS_NewFloat64(ctx, bb.extentZ()));
    return obj;
}

// ---------------------------------------------------------------------------
// Wrap helper — creates a JS Mesh from MeshData
// ---------------------------------------------------------------------------

static JSValue wrapMesh(JSContext* ctx, bromesh::MeshData&& data) {
    return qjsbind::wrap<MW>(ctx,
        new MW{std::make_unique<bromesh::MeshData>(std::move(data))});
}

// ---------------------------------------------------------------------------
// Raw static functions (complex arg parsing that doesn't fit typed lambdas)
// ---------------------------------------------------------------------------

static JSValue js_heightmapGrid(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "heightmapGrid requires (heights, gridW, gridH)");
    std::vector<float> heights;
    if (!readFloatArrayVal(ctx, argv[0], heights)) return JS_ThrowTypeError(ctx, "heights must be Float32Array");
    int32_t gw = 0, gh = 0; double cs = 1.0;
    JS_ToInt32(ctx, &gw, argv[1]);
    JS_ToInt32(ctx, &gh, argv[2]);
    if (argc > 3) JS_ToFloat64(ctx, &cs, argv[3]);
    return wrapMesh(ctx, bromesh::heightmapGrid(heights.data(), gw, gh, (float)cs));
}

static JSValue js_splitByPlane(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "splitByPlane requires a Mesh argument");
    auto* w = qjsbind::unwrap<MW>(ctx, argv[0]);
    if (!w || !w->data) return JS_ThrowTypeError(ctx, "first argument must be a Mesh");
    double nx = 0, ny = 1, nz = 0, offset = 0;
    if (argc > 1) JS_ToFloat64(ctx, &nx, argv[1]);
    if (argc > 2) JS_ToFloat64(ctx, &ny, argv[2]);
    if (argc > 3) JS_ToFloat64(ctx, &nz, argv[3]);
    if (argc > 4) JS_ToFloat64(ctx, &offset, argv[4]);
    auto [front, back] = bromesh::splitByPlane(*w->data, (float)nx, (float)ny, (float)nz, (float)offset);
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, wrapMesh(ctx, bromesh::computeCreaseNormals(front)));
    JS_SetPropertyUint32(ctx, arr, 1, wrapMesh(ctx, bromesh::computeCreaseNormals(back)));
    return arr;
}

static JSValue js_marchingCubes(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "marchingCubes requires (field, gridX, gridY, gridZ)");
    std::vector<float> field;
    if (!readFloatArrayVal(ctx, argv[0], field)) return JS_ThrowTypeError(ctx, "field must be Float32Array");
    int32_t gx = 0, gy = 0, gz = 0; double iso = 0, cs = 1;
    JS_ToInt32(ctx, &gx, argv[1]); JS_ToInt32(ctx, &gy, argv[2]); JS_ToInt32(ctx, &gz, argv[3]);
    if (argc > 4) JS_ToFloat64(ctx, &iso, argv[4]);
    if (argc > 5) JS_ToFloat64(ctx, &cs, argv[5]);
    return wrapMesh(ctx, bromesh::marchingCubes(field.data(), gx, gy, gz, (float)iso, (float)cs));
}

static JSValue js_dualContour(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "dualContour requires (field, gridX, gridY, gridZ)");
    std::vector<float> field;
    if (!readFloatArrayVal(ctx, argv[0], field)) return JS_ThrowTypeError(ctx, "field must be Float32Array");
    int32_t gx = 0, gy = 0, gz = 0; double iso = 0, cs = 1;
    JS_ToInt32(ctx, &gx, argv[1]); JS_ToInt32(ctx, &gy, argv[2]); JS_ToInt32(ctx, &gz, argv[3]);
    if (argc > 4) JS_ToFloat64(ctx, &iso, argv[4]);
    if (argc > 5) JS_ToFloat64(ctx, &cs, argv[5]);
    return wrapMesh(ctx, bromesh::dualContour(field.data(), gx, gy, gz, (float)iso, (float)cs));
}

static JSValue js_greedyMesh(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "greedyMesh requires (voxels, gridX, gridY, gridZ)");
    std::vector<uint8_t> voxels;
    if (!readUint8ArrayVal(ctx, argv[0], voxels)) return JS_ThrowTypeError(ctx, "voxels must be Uint8Array");
    int32_t gx = 0, gy = 0, gz = 0; double cs = 1;
    JS_ToInt32(ctx, &gx, argv[1]); JS_ToInt32(ctx, &gy, argv[2]); JS_ToInt32(ctx, &gz, argv[3]);
    if (argc > 4) JS_ToFloat64(ctx, &cs, argv[4]);
    std::vector<float> palette;
    int palCount = 0;
    if (argc > 5 && JS_IsObject(argv[5])) {
        readFloatArrayVal(ctx, argv[5], palette);
        int32_t pc = (int32_t)(palette.size() / 4);
        if (argc > 6) JS_ToInt32(ctx, &pc, argv[6]);
        palCount = pc;
    }
    int32_t filterMat = -1;
    if (argc > 7) JS_ToInt32(ctx, &filterMat, argv[7]);
    if (palette.empty() && argc > 5 && JS_IsNumber(argv[5])) JS_ToInt32(ctx, &filterMat, argv[5]);
    return wrapMesh(ctx, bromesh::greedyMesh(voxels.data(), gx, gy, gz, (float)cs,
        palette.empty() ? nullptr : palette.data(), palCount, filterMat));
}

static JSValue js_surfaceNets(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "surfaceNets requires (field, gridX, gridY, gridZ)");
    std::vector<float> field;
    if (!readFloatArrayVal(ctx, argv[0], field)) return JS_ThrowTypeError(ctx, "field must be Float32Array");
    int32_t gx=0, gy=0, gz=0; double iso=0, cs=1;
    JS_ToInt32(ctx, &gx, argv[1]); JS_ToInt32(ctx, &gy, argv[2]); JS_ToInt32(ctx, &gz, argv[3]);
    if (argc > 4) JS_ToFloat64(ctx, &iso, argv[4]);
    if (argc > 5) JS_ToFloat64(ctx, &cs,  argv[5]);
    return wrapMesh(ctx, bromesh::surfaceNets(field.data(), gx, gy, gz, (float)iso, (float)cs));
}

static JSValue js_transvoxel(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "transvoxel requires (field, gridSize, lod, neighborLods, [iso], [cs])");
    std::vector<float> field;
    if (!readFloatArrayVal(ctx, argv[0], field)) return JS_ThrowTypeError(ctx, "field must be Float32Array");
    int32_t grid=0, lod=0; double iso=0, cs=1;
    JS_ToInt32(ctx, &grid, argv[1]);
    JS_ToInt32(ctx, &lod,  argv[2]);
    int neighbors[6] = {-1,-1,-1,-1,-1,-1};
    if (JS_IsArray(argv[3])) {
        for (int i = 0; i < 6; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, argv[3], i);
            if (!JS_IsUndefined(e)) JS_ToInt32(ctx, &neighbors[i], e);
            JS_FreeValue(ctx, e);
        }
    }
    if (argc > 4) JS_ToFloat64(ctx, &iso, argv[4]);
    if (argc > 5) JS_ToFloat64(ctx, &cs,  argv[5]);
    return wrapMesh(ctx, bromesh::transvoxel(field.data(), grid, lod, neighbors, (float)iso, (float)cs));
}

static JSValue js_decode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "decode requires an EncodedMesh object");
    JSValueConst enc = argv[0];
    bromesh::EncodedMesh e;
    JSValue v;
    v = JS_GetPropertyStr(ctx, enc, "vertexData");
    if (!readUint8ArrayVal(ctx, v, e.vertexData)) { JS_FreeValue(ctx, v); return JS_ThrowTypeError(ctx, "vertexData must be Uint8Array"); }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, enc, "indexData");
    if (!readUint8ArrayVal(ctx, v, e.indexData)) { JS_FreeValue(ctx, v); return JS_ThrowTypeError(ctx, "indexData must be Uint8Array"); }
    JS_FreeValue(ctx, v);
    int32_t vc=0, vs=0, ic=0;
    v = JS_GetPropertyStr(ctx, enc, "vertexCount"); JS_ToInt32(ctx, &vc, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, enc, "vertexSize");  JS_ToInt32(ctx, &vs, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, enc, "indexCount");  JS_ToInt32(ctx, &ic, v); JS_FreeValue(ctx, v);
    e.vertexCount = (size_t)vc;
    e.vertexSize  = (size_t)vs;
    e.indexCount  = (size_t)ic;
    bool hasN=false, hasU=false, hasC=false;
    v = JS_GetPropertyStr(ctx, enc, "hasNormals"); hasN = JS_ToBool(ctx, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, enc, "hasUVs");     hasU = JS_ToBool(ctx, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, enc, "hasColors");  hasC = JS_ToBool(ctx, v); JS_FreeValue(ctx, v);
    return wrapMesh(ctx, bromesh::decodeMesh(e, hasN, hasU, hasC));
}

static JSValue js_reconstruct(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "reconstruct requires a Mesh (point cloud)");
    auto* w = qjsbind::unwrap<MW>(ctx, argv[0]);
    if (!w || !w->data) return JS_ThrowTypeError(ctx, "argument must be a Mesh");
    bromesh::ReconstructParams params;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v;
        v = JS_GetPropertyStr(ctx, argv[1], "gridResolution");
        if (!JS_IsUndefined(v)) { int32_t x; JS_ToInt32(ctx, &x, v); params.gridResolution = x; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "supportRadius");
        if (!JS_IsUndefined(v)) { double x; JS_ToFloat64(ctx, &x, v); params.supportRadius = (float)x; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "isoLevel");
        if (!JS_IsUndefined(v)) { double x; JS_ToFloat64(ctx, &x, v); params.isoLevel = (float)x; }
        JS_FreeValue(ctx, v);
    }
    return wrapMesh(ctx, bromesh::reconstructFromPointCloud(*w->data, params));
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void MeshBindings::install(JSContext* ctx) {
    qjsbind::Class<MW>(ctx, "Mesh")

    // ── Constructor ─────────────────────────────────────────────────────
    .constructor([](JSContext* ctx, int argc, JSValueConst* argv) -> MW* {
        auto md = std::make_unique<bromesh::MeshData>();
        if (argc > 0 && JS_IsObject(argv[0])) {
            readFloatArray(ctx, argv[0], "positions", md->positions);
            readFloatArray(ctx, argv[0], "normals", md->normals);
            readFloatArray(ctx, argv[0], "uvs", md->uvs);
            readFloatArray(ctx, argv[0], "colors", md->colors);
            readUint32Array(ctx, argv[0], "indices", md->indices);
        }
        return new MW{std::move(md)};
    })

    // ── TypedArray properties (getter + setter) ─────────────────────────
    .prop("positions",
        [](MW* w, JSContext* ctx) -> JSValue {
            return w->data ? makeFloat32Array(ctx, w->data->positions) : JS_UNDEFINED;
        },
        [](MW* w, JSContext* ctx, JSValue val) {
            if (w->data) readFloatArrayVal(ctx, val, w->data->positions);
        })
    .prop("normals",
        [](MW* w, JSContext* ctx) -> JSValue {
            return w->data ? makeFloat32Array(ctx, w->data->normals) : JS_UNDEFINED;
        },
        [](MW* w, JSContext* ctx, JSValue val) {
            if (w->data) readFloatArrayVal(ctx, val, w->data->normals);
        })
    .prop("uvs",
        [](MW* w, JSContext* ctx) -> JSValue {
            return w->data ? makeFloat32Array(ctx, w->data->uvs) : JS_UNDEFINED;
        },
        [](MW* w, JSContext* ctx, JSValue val) {
            if (w->data) readFloatArrayVal(ctx, val, w->data->uvs);
        })
    .prop("colors",
        [](MW* w, JSContext* ctx) -> JSValue {
            return w->data ? makeFloat32Array(ctx, w->data->colors) : JS_UNDEFINED;
        },
        [](MW* w, JSContext* ctx, JSValue val) {
            if (w->data) readFloatArrayVal(ctx, val, w->data->colors);
        })
    .prop("indices",
        [](MW* w, JSContext* ctx) -> JSValue {
            return w->data ? makeUint32Array(ctx, w->data->indices) : JS_UNDEFINED;
        },
        [](MW* w, JSContext* ctx, JSValue val) {
            if (w->data) readUint32ArrayVal(ctx, val, w->data->indices);
        })

    // ── Read-only properties ────────────────────────────────────────────
    .get("vertexCount",    [](MW* w) { return (int)(w->data ? w->data->vertexCount() : 0); })
    .get("triangleCount",  [](MW* w) { return (int)(w->data ? w->data->triangleCount() : 0); })
    .get("hasNormals",     [](MW* w) { return w->data && w->data->hasNormals(); })
    .get("hasUVs",         [](MW* w) { return w->data && w->data->hasUVs(); })
    .get("hasColors",      [](MW* w) { return w->data && w->data->hasColors(); })
    .get("empty",          [](MW* w) { return !w->data || w->data->empty(); })

    // ── Clone ───────────────────────────────────────────────────────────
    .method("clone", [](MW* w, JSContext* ctx) -> JSValue {
        return w->data ? wrapMesh(ctx, bromesh::MeshData(*w->data)) : JS_UNDEFINED;
    })

    // ── Transforms ──────────────────────────────────────────────────────
    .method("translate", [](MW* w, double dx, double dy, double dz) {
        if (!w->data) return;
        for (size_t i = 0; i < w->data->positions.size(); i += 3) {
            w->data->positions[i]     += (float)dx;
            w->data->positions[i + 1] += (float)dy;
            w->data->positions[i + 2] += (float)dz;
        }
    }, qjsbind::returns_this)

    .method("scale", [](MW* w, double sx, std::optional<double> sy_opt, std::optional<double> sz_opt) {
        if (!w->data) return;
        float fsx = (float)sx;
        float fsy = sy_opt ? (float)*sy_opt : fsx;
        float fsz = sz_opt ? (float)*sz_opt : fsx;
        auto* md = w->data.get();
        for (size_t i = 0; i < md->positions.size(); i += 3) {
            md->positions[i]     *= fsx;
            md->positions[i + 1] *= fsy;
            md->positions[i + 2] *= fsz;
        }
        if (fsx != fsy || fsy != fsz) bromesh::computeNormals(*md);
    })

    .method("rotate", [](MW* w, double ax, double ay, double az, double angle) {
        if (!w->data) return;
        float fax = (float)ax, fay = (float)ay, faz = (float)az;
        float len = std::sqrt(fax*fax + fay*fay + faz*faz);
        if (len < 1e-8f) return;
        fax /= len; fay /= len; faz /= len;
        float c = std::cos((float)angle), s = std::sin((float)angle), t = 1.0f - c;
        float m00=t*fax*fax+c,    m01=t*fax*fay-s*faz, m02=t*fax*faz+s*fay;
        float m10=t*fax*fay+s*faz, m11=t*fay*fay+c,    m12=t*fay*faz-s*fax;
        float m20=t*fax*faz-s*fay, m21=t*fay*faz+s*fax, m22=t*faz*faz+c;
        auto rot = [&](std::vector<float>& v) {
            for (size_t i = 0; i < v.size(); i += 3) {
                float x = v[i], y = v[i+1], z = v[i+2];
                v[i]   = m00*x + m01*y + m02*z;
                v[i+1] = m10*x + m11*y + m12*z;
                v[i+2] = m20*x + m21*y + m22*z;
            }
        };
        rot(w->data->positions);
        if (w->data->hasNormals()) rot(w->data->normals);
    }, qjsbind::returns_this)

    .method("center", [](MW* w) {
        if (!w->data || w->data->positions.empty()) return;
        auto bbox = bromesh::computeBBox(*w->data);
        float cx = bbox.centerX(), cy = bbox.centerY(), cz = bbox.centerZ();
        for (size_t i = 0; i < w->data->positions.size(); i += 3) {
            w->data->positions[i]     -= cx;
            w->data->positions[i + 1] -= cy;
            w->data->positions[i + 2] -= cz;
        }
    }, qjsbind::returns_this)

    .method("mirror", [](MW* w, int axis) {
        if (!w->data || axis < 0 || axis > 2) return;
        auto* md = w->data.get();
        for (size_t i = 0; i < md->positions.size(); i += 3)
            md->positions[i + axis] = -md->positions[i + axis];
        if (md->hasNormals())
            for (size_t i = 0; i < md->normals.size(); i += 3)
                md->normals[i + axis] = -md->normals[i + axis];
        for (size_t i = 0; i + 2 < md->indices.size(); i += 3)
            std::swap(md->indices[i + 1], md->indices[i + 2]);
    }, qjsbind::returns_this)

    .method("transform", [](MW* w, JSContext* ctx, JSValue matArr) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        std::vector<float> mat;
        if (!readFloatArrayVal(ctx, matArr, mat) || mat.size() < 16) return JS_UNDEFINED;
        const float* m = mat.data();
        auto* md = w->data.get();
        for (size_t i = 0; i < md->positions.size(); i += 3) {
            float x = md->positions[i], y = md->positions[i+1], z = md->positions[i+2];
            md->positions[i]   = m[0]*x + m[4]*y + m[8]*z  + m[12];
            md->positions[i+1] = m[1]*x + m[5]*y + m[9]*z  + m[13];
            md->positions[i+2] = m[2]*x + m[6]*y + m[10]*z + m[14];
        }
        if (md->hasNormals()) {
            for (size_t i = 0; i < md->normals.size(); i += 3) {
                float x = md->normals[i], y = md->normals[i+1], z = md->normals[i+2];
                md->normals[i]   = m[0]*x + m[4]*y + m[8]*z;
                md->normals[i+1] = m[1]*x + m[5]*y + m[9]*z;
                md->normals[i+2] = m[2]*x + m[6]*y + m[10]*z;
                float len = std::sqrt(md->normals[i]*md->normals[i] +
                                      md->normals[i+1]*md->normals[i+1] +
                                      md->normals[i+2]*md->normals[i+2]);
                if (len > 1e-8f) {
                    md->normals[i] /= len; md->normals[i+1] /= len; md->normals[i+2] /= len;
                }
            }
        }
        return JS_UNDEFINED; // returns_this handles the return
    }, qjsbind::returns_this)

    // ── Normals ─────────────────────────────────────────────────────────
    .method("computeNormals", [](MW* w) {
        if (w->data) bromesh::computeNormals(*w->data);
    }, qjsbind::returns_this)

    .method("computeFlatNormals", [](MW* w, JSContext* ctx) -> JSValue {
        return w->data ? wrapMesh(ctx, bromesh::computeFlatNormals(*w->data)) : JS_UNDEFINED;
    })

    .method("computeTangents", [](MW* w, JSContext* ctx) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        return makeFloat32Array(ctx, bromesh::computeTangents(*w->data));
    })

    // ── Simplification ──────────────────────────────────────────────────
    .method("simplify", [](MW* w, double ratio, std::optional<double> error) {
        if (w->data) *w->data = bromesh::simplify(*w->data, (float)ratio, (float)error.value_or(0.01));
    }, qjsbind::returns_this)

    .method("simplifyWithAttributes", [](MW* w, double ratio, std::optional<double> error,
                                          std::optional<double> uvW, std::optional<double> nW) {
        if (w->data) *w->data = bromesh::simplifyWithAttributes(*w->data,
            (float)ratio, (float)error.value_or(0.01), (float)uvW.value_or(1.0), (float)nW.value_or(0.5));
    }, qjsbind::returns_this)

    .method("simplifyToTriangleCount", [](MW* w, int target, std::optional<double> error) {
        if (w->data) *w->data = bromesh::simplifyToTriangleCount(*w->data, (size_t)target, (float)error.value_or(0.01));
    }, qjsbind::returns_this)

    .method("generateLODChain", [](MW* w, JSContext* ctx, JSValue ratiosArr) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        std::vector<float> ratios;
        if (!readFloatArrayVal(ctx, ratiosArr, ratios) || ratios.empty()) return JS_UNDEFINED;
        auto lods = bromesh::generateLODChain(*w->data, ratios.data(), (int)ratios.size());
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < lods.size(); i++)
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, wrapMesh(ctx, std::move(lods[i])));
        return arr;
    })

    // ── Subdivision ─────────────────────────────────────────────────────
    .method("subdivideLoop", [](MW* w, std::optional<int> iter) {
        if (w->data) { auto welded = bromesh::weldVertices(*w->data); *w->data = bromesh::subdivideLoop(welded, iter.value_or(1)); }
    }, qjsbind::returns_this)

    .method("subdivideCatmullClark", [](MW* w, std::optional<int> iter) {
        if (w->data) { auto welded = bromesh::weldVertices(*w->data); *w->data = bromesh::subdivideCatmullClark(welded, iter.value_or(1)); }
    }, qjsbind::returns_this)

    .method("subdivideMidpoint", [](MW* w, std::optional<int> iter) {
        if (w->data) { auto welded = bromesh::weldVertices(*w->data); *w->data = bromesh::subdivideMidpoint(welded, iter.value_or(1)); }
    }, qjsbind::returns_this)

    // ── Smoothing ───────────────────────────────────────────────────────
    .method("smoothLaplacian", [](MW* w, std::optional<double> lambda, std::optional<int> iter) {
        if (w->data) bromesh::smoothLaplacian(*w->data, (float)lambda.value_or(0.5), iter.value_or(1));
    }, qjsbind::returns_this)

    .method("smoothTaubin", [](MW* w, std::optional<double> lambda, std::optional<double> mu,
                                std::optional<int> iter) {
        if (w->data) bromesh::smoothTaubin(*w->data, (float)lambda.value_or(0.5), (float)mu.value_or(-0.53), iter.value_or(1));
    }, qjsbind::returns_this)

    // ── Optimization ────────────────────────────────────────────────────
    .method("optimizeVertexCache", [](MW* w) {
        if (w->data) bromesh::optimizeVertexCache(*w->data);
    }, qjsbind::returns_this)

    .method("optimizeVertexFetch", [](MW* w) {
        if (w->data) bromesh::optimizeVertexFetch(*w->data);
    }, qjsbind::returns_this)

    .method("optimizeOverdraw", [](MW* w, std::optional<double> thresh) {
        if (w->data) bromesh::optimizeOverdraw(*w->data, (float)thresh.value_or(1.05));
    }, qjsbind::returns_this)

    // ── Analysis ────────────────────────────────────────────────────────
    .method("computeBBox", [](MW* w, JSContext* ctx) -> JSValue {
        return w->data ? makeBBox(ctx, bromesh::computeBBox(*w->data)) : JS_UNDEFINED;
    })

    .method("isManifold", [](MW* w) { return w->data ? bromesh::isManifold(*w->data) : false; })

    .method("computeVolume", [](MW* w) { return w->data ? bromesh::computeVolume(*w->data) : 0.0; })

    .method("raycast", [](MW* w, JSContext* ctx, JSValue origin, JSValue direction,
                           std::optional<double> maxDist) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        float o[3], d[3];
        readVec3(ctx, origin, o);
        readVec3(ctx, direction, d);
        return makeRayHit(ctx, bromesh::raycast(*w->data, o, d, (float)maxDist.value_or(0.0)));
    })

    .method("raycastAll", [](MW* w, JSContext* ctx, JSValue origin, JSValue direction,
                              std::optional<double> maxDist) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        float o[3], d[3];
        readVec3(ctx, origin, o);
        readVec3(ctx, direction, d);
        auto hits = bromesh::raycastAll(*w->data, o, d, (float)maxDist.value_or(0.0));
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < hits.size(); i++)
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, makeRayHit(ctx, hits[i]));
        return arr;
    })

    .method("raycastTest", [](MW* w, JSContext* ctx, JSValue origin, JSValue direction,
                               std::optional<double> maxDist) -> JSValue {
        if (!w->data) return JS_FALSE;
        float o[3], d[3];
        readVec3(ctx, origin, o);
        readVec3(ctx, direction, d);
        return JS_NewBool(ctx, bromesh::raycastTest(*w->data, o, d, (float)maxDist.value_or(0.0)));
    })

    .method("closestPoint", [](MW* w, JSContext* ctx, JSValue point) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        float pt[3];
        readVec3(ctx, point, pt);
        return makeRayHit(ctx, bromesh::closestPoint(*w->data, pt));
    })

    .method("hasSelfIntersections", [](MW* w) { return w->data ? bromesh::hasSelfIntersections(*w->data) : false; })

    .method("findSelfIntersections", [](MW* w, JSContext* ctx) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        auto pairs = bromesh::findSelfIntersections(*w->data);
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < pairs.size(); i++) {
            JSValue obj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, obj, "triA", JS_NewInt32(ctx, (int32_t)pairs[i].triA));
            JS_SetPropertyStr(ctx, obj, "triB", JS_NewInt32(ctx, (int32_t)pairs[i].triB));
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, obj);
        }
        return arr;
    })

    .method("intersectsMesh", [](MW* w, JSContext* ctx, JSValue other) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        auto* otherMD = qjsbind::unwrap<MW>(ctx, other);
        if (!otherMD || !otherMD->data) return JS_ThrowTypeError(ctx, "argument must be a Mesh");
        return JS_NewBool(ctx, bromesh::meshesIntersect(*w->data, *otherMD->data));
    })

    // ── UV ──────────────────────────────────────────────────────────────
    .method("unwrapUVs", [](MW* w, JSContext* ctx) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        auto result = bromesh::unwrapUVs(*w->data);
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "atlasWidth", JS_NewInt32(ctx, result.atlasWidth));
        JS_SetPropertyStr(ctx, obj, "atlasHeight", JS_NewInt32(ctx, result.atlasHeight));
        JS_SetPropertyStr(ctx, obj, "chartCount", JS_NewInt32(ctx, result.chartCount));
        JS_SetPropertyStr(ctx, obj, "success", JS_NewBool(ctx, result.success));
        return obj;
    })

    .method("projectUVs", [](MW* w, std::string type, std::optional<double> scale) {
        if (!w->data) return;
        bromesh::ProjectionType pt = bromesh::ProjectionType::Box;
        if (type == "box")              pt = bromesh::ProjectionType::Box;
        else if (type == "planarXY")    pt = bromesh::ProjectionType::PlanarXY;
        else if (type == "planarXZ")    pt = bromesh::ProjectionType::PlanarXZ;
        else if (type == "planarYZ")    pt = bromesh::ProjectionType::PlanarYZ;
        else if (type == "cylindrical") pt = bromesh::ProjectionType::Cylindrical;
        else if (type == "spherical")   pt = bromesh::ProjectionType::Spherical;
        bromesh::projectUVs(*w->data, pt, (float)scale.value_or(1.0));
    }, qjsbind::returns_this)

    // ── Save I/O ────────────────────────────────────────────────────────
    .method("saveGLTF", [](MW* w, std::string path) { return w->data ? bromesh::saveGLTF(*w->data, path) : false; })
    .method("saveOBJ",  [](MW* w, std::string path) { return w->data ? bromesh::saveOBJ(*w->data, path) : false; })
    .method("savePLY",  [](MW* w, std::string path) { return w->data ? bromesh::savePLY(*w->data, path) : false; })
    .method("saveSTL",  [](MW* w, std::string path) { return w->data ? bromesh::saveSTL(*w->data, path) : false; })

    // ── Remesh / repair / weld / crease ────────────────────────────────
    .method("remeshIsotropic", [](MW* w, std::optional<double> edgeLen, std::optional<int> iter) {
        if (w->data) *w->data = bromesh::remeshIsotropic(*w->data, (float)edgeLen.value_or(0.0), iter.value_or(5));
    }, qjsbind::returns_this)

    .method("weld", [](MW* w, std::optional<double> epsilon) {
        if (w->data) *w->data = bromesh::weldVertices(*w->data, (float)epsilon.value_or(1e-5));
    }, qjsbind::returns_this)

    .method("computeCreaseNormals", [](MW* w, std::optional<double> angleDeg) {
        if (w->data) *w->data = bromesh::computeCreaseNormals(*w->data, (float)angleDeg.value_or(30.0));
    }, qjsbind::returns_this)

    .method("removeDegenerateTriangles", [](MW* w, std::optional<double> eps) {
        if (w->data) *w->data = bromesh::removeDegenerateTriangles(*w->data, (float)eps.value_or(1e-8));
    }, qjsbind::returns_this)

    .method("removeDuplicateTriangles", [](MW* w) {
        if (w->data) *w->data = bromesh::removeDuplicateTriangles(*w->data);
    }, qjsbind::returns_this)

    .method("fillHoles", [](MW* w, std::optional<int> maxEdges) {
        if (w->data) *w->data = bromesh::fillHoles(*w->data, maxEdges.value_or(64));
    }, qjsbind::returns_this)

    .method("splitComponents", [](MW* w, JSContext* ctx) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        auto comps = bromesh::splitConnectedComponents(*w->data);
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < comps.size(); i++)
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, wrapMesh(ctx, std::move(comps[i])));
        return arr;
    })

    .method("shrinkwrap", [](MW* w, JSContext* ctx, JSValue targetVal,
                              std::optional<std::string> mode, std::optional<double> maxDist,
                              std::optional<double> offset, std::optional<JSValue> axisArr) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        auto* t = qjsbind::unwrap<MW>(ctx, targetVal);
        if (!t || !t->data) return JS_ThrowTypeError(ctx, "target must be a Mesh");
        auto m = bromesh::ShrinkwrapMode::Nearest;
        if (mode) {
            if (*mode == "projectAlongNormal")     m = bromesh::ShrinkwrapMode::ProjectAlongNormal;
            else if (*mode == "projectAlongAxis")  m = bromesh::ShrinkwrapMode::ProjectAlongAxis;
        }
        float axis[3] = {0,1,0};
        bool useAxis = false;
        if (axisArr && JS_IsObject(*axisArr)) { readVec3(ctx, *axisArr, axis); useAxis = true; }
        bromesh::shrinkwrap(*w->data, *t->data, m,
                            (float)maxDist.value_or(0.0), (float)offset.value_or(0.0),
                            useAxis ? axis : nullptr);
        return JS_UNDEFINED;
    }, qjsbind::returns_this)

    // ── Surface sampling / metrics ─────────────────────────────────────
    .method("sampleSurface", [](MW* w, JSContext* ctx, int numSamples, std::optional<int> seed) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        return wrapMesh(ctx, bromesh::sampleSurface(*w->data, (size_t)numSamples, (uint32_t)seed.value_or(0)));
    })

    .method("surfaceArea", [](MW* w) {
        return w->data ? (double)bromesh::computeSurfaceArea(*w->data) : 0.0;
    })

    .method("triangleAreas", [](MW* w, JSContext* ctx) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        return makeFloat32Array(ctx, bromesh::computeTriangleAreas(*w->data));
    })

    // ── Vertex-space baking ─────────────────────────────────────────────
    .method("bakeAmbientOcclusion", [](MW* w, std::optional<int> numRays, std::optional<double> maxDist) {
        if (w->data) bromesh::bakeAmbientOcclusion(*w->data, numRays.value_or(64), (float)maxDist.value_or(0.0));
    }, qjsbind::returns_this)

    .method("bakeCurvature", [](MW* w, std::optional<double> scale) {
        if (w->data) bromesh::bakeCurvature(*w->data, (float)scale.value_or(1.0));
    }, qjsbind::returns_this)

    .method("bakeThickness", [](MW* w, std::optional<int> numRays, std::optional<double> maxDist) {
        if (w->data) bromesh::bakeThickness(*w->data, numRays.value_or(32), (float)maxDist.value_or(0.0));
    }, qjsbind::returns_this)

    // ── Texture-space baking ────────────────────────────────────────────
    .method("bakeAOToTexture", [](MW* w, JSContext* ctx, int texW, int texH,
                                   std::optional<int> numRays, std::optional<double> maxDist) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        return makeTextureBuffer(ctx, bromesh::bakeAmbientOcclusionToTexture(
            *w->data, texW, texH, numRays.value_or(64), (float)maxDist.value_or(0.0)));
    })
    .method("bakeCurvatureToTexture", [](MW* w, JSContext* ctx, int texW, int texH, std::optional<double> scale) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        return makeTextureBuffer(ctx, bromesh::bakeCurvatureToTexture(
            *w->data, texW, texH, (float)scale.value_or(1.0)));
    })
    .method("bakeThicknessToTexture", [](MW* w, JSContext* ctx, int texW, int texH,
                                          std::optional<int> numRays, std::optional<double> maxDist) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        return makeTextureBuffer(ctx, bromesh::bakeThicknessToTexture(
            *w->data, texW, texH, numRays.value_or(32), (float)maxDist.value_or(0.0)));
    })
    .method("bakeNormalsToTexture", [](MW* w, JSContext* ctx, int texW, int texH) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        return makeTextureBuffer(ctx, bromesh::bakeNormalsToTexture(*w->data, texW, texH));
    })
    .method("bakePositionToTexture", [](MW* w, JSContext* ctx, int texW, int texH) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        return makeTextureBuffer(ctx, bromesh::bakePositionToTexture(*w->data, texW, texH));
    })
    .method("bakeNormalsFromReference", [](MW* w, JSContext* ctx, JSValue refVal,
                                            int texW, int texH, std::optional<double> searchDist) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        auto* r = qjsbind::unwrap<MW>(ctx, refVal);
        if (!r || !r->data) return JS_ThrowTypeError(ctx, "reference must be a Mesh");
        return makeTextureBuffer(ctx, bromesh::bakeNormalsFromReference(
            *w->data, *r->data, texW, texH, (float)searchDist.value_or(0.0)));
    })
    .method("bakeAOFromReference", [](MW* w, JSContext* ctx, JSValue refVal,
                                       int texW, int texH,
                                       std::optional<int> numRays, std::optional<double> maxDist) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        auto* r = qjsbind::unwrap<MW>(ctx, refVal);
        if (!r || !r->data) return JS_ThrowTypeError(ctx, "reference must be a Mesh");
        return makeTextureBuffer(ctx, bromesh::bakeAOFromReference(
            *w->data, *r->data, texW, texH, numRays.value_or(64), (float)maxDist.value_or(0.0)));
    })

    // ── Convex ──────────────────────────────────────────────────────────
    .method("convexHull", [](MW* w, JSContext* ctx) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        return wrapMesh(ctx, bromesh::convexHull(*w->data));
    })

    .method("convexDecomposition", [](MW* w, JSContext* ctx, std::optional<JSValue> paramsObj) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        bromesh::ConvexDecompParams p;
        if (paramsObj && JS_IsObject(*paramsObj)) {
            JSValue v;
            v = JS_GetPropertyStr(ctx, *paramsObj, "maxHulls");
            if (!JS_IsUndefined(v)) { int32_t x; JS_ToInt32(ctx, &x, v); p.maxHulls = x; }
            JS_FreeValue(ctx, v);
            v = JS_GetPropertyStr(ctx, *paramsObj, "maxVerticesPerHull");
            if (!JS_IsUndefined(v)) { int32_t x; JS_ToInt32(ctx, &x, v); p.maxVerticesPerHull = x; }
            JS_FreeValue(ctx, v);
            v = JS_GetPropertyStr(ctx, *paramsObj, "resolution");
            if (!JS_IsUndefined(v)) { double x; JS_ToFloat64(ctx, &x, v); p.resolution = (float)x; }
            JS_FreeValue(ctx, v);
            v = JS_GetPropertyStr(ctx, *paramsObj, "minVolumePerHull");
            if (!JS_IsUndefined(v)) { double x; JS_ToFloat64(ctx, &x, v); p.minVolumePerHull = (float)x; }
            JS_FreeValue(ctx, v);
        }
        auto hulls = bromesh::convexDecomposition(*w->data, p);
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < hulls.size(); i++)
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, wrapMesh(ctx, std::move(hulls[i])));
        return arr;
    })

    // ── UV metrics ──────────────────────────────────────────────────────
    .method("computeUVDistortion", [](MW* w, JSContext* ctx) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        auto dist = bromesh::computeUVDistortion(*w->data);
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < dist.size(); i++) {
            JSValue o = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, o, "stretch",         JS_NewFloat64(ctx, dist[i].stretch));
            JS_SetPropertyStr(ctx, o, "areaDistortion",  JS_NewFloat64(ctx, dist[i].areaDistortion));
            JS_SetPropertyStr(ctx, o, "angleDistortion", JS_NewFloat64(ctx, dist[i].angleDistortion));
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
        }
        return arr;
    })

    .method("measureUVQuality", [](MW* w, JSContext* ctx) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        auto m = bromesh::measureUVQuality(*w->data);
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "avgStretch",         JS_NewFloat64(ctx, m.avgStretch));
        JS_SetPropertyStr(ctx, o, "maxStretch",         JS_NewFloat64(ctx, m.maxStretch));
        JS_SetPropertyStr(ctx, o, "avgAreaDistortion",  JS_NewFloat64(ctx, m.avgAreaDistortion));
        JS_SetPropertyStr(ctx, o, "maxAreaDistortion",  JS_NewFloat64(ctx, m.maxAreaDistortion));
        JS_SetPropertyStr(ctx, o, "avgAngleDistortion", JS_NewFloat64(ctx, m.avgAngleDistortion));
        JS_SetPropertyStr(ctx, o, "maxAngleDistortion", JS_NewFloat64(ctx, m.maxAngleDistortion));
        JS_SetPropertyStr(ctx, o, "uvSpaceUsage",       JS_NewFloat64(ctx, m.uvSpaceUsage));
        JS_SetPropertyStr(ctx, o, "triangleCount",      JS_NewInt32(ctx, (int32_t)m.triangleCount));
        return o;
    })

    // ── Analyze (meshopt-backed stats) ─────────────────────────────────
    .method("analyzeVertexCache", [](MW* w, JSContext* ctx, std::optional<int> cacheSize) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        auto s = bromesh::analyzeVertexCache(*w->data, (unsigned)cacheSize.value_or(16));
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "verticesTransformed", JS_NewInt64(ctx, (int64_t)s.verticesTransformed));
        JS_SetPropertyStr(ctx, o, "warpsExecuted",       JS_NewInt64(ctx, (int64_t)s.warpsExecuted));
        JS_SetPropertyStr(ctx, o, "acmr",                JS_NewFloat64(ctx, s.acmr));
        JS_SetPropertyStr(ctx, o, "atvr",                JS_NewFloat64(ctx, s.atvr));
        return o;
    })

    .method("analyzeVertexFetch", [](MW* w, JSContext* ctx, std::optional<int> vertexSize) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        auto s = bromesh::analyzeVertexFetch(*w->data, (size_t)vertexSize.value_or(32));
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "bytesFetched", JS_NewInt64(ctx, (int64_t)s.bytesFetched));
        JS_SetPropertyStr(ctx, o, "overfetch",    JS_NewFloat64(ctx, s.overfetch));
        return o;
    })

    .method("analyzeOverdraw", [](MW* w, JSContext* ctx) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        auto s = bromesh::analyzeOverdraw(*w->data);
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "pixelsCovered", JS_NewInt64(ctx, (int64_t)s.pixelsCovered));
        JS_SetPropertyStr(ctx, o, "pixelsShaded",  JS_NewInt64(ctx, (int64_t)s.pixelsShaded));
        JS_SetPropertyStr(ctx, o, "overdraw",      JS_NewFloat64(ctx, s.overdraw));
        return o;
    })

    // ── Spatial sort / shadow indices ──────────────────────────────────
    .method("spatialSortTriangles", [](MW* w) {
        if (w->data) bromesh::spatialSortTriangles(*w->data);
    }, qjsbind::returns_this)
    .method("spatialSortVertices", [](MW* w) {
        if (w->data) bromesh::spatialSortVertices(*w->data);
    }, qjsbind::returns_this)
    .method("generateShadowIndexBuffer", [](MW* w, JSContext* ctx) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        return makeUint32Array(ctx, bromesh::generateShadowIndexBuffer(*w->data));
    })

    // ── Meshlets ───────────────────────────────────────────────────────
    .method("buildMeshlets", [](MW* w, JSContext* ctx, std::optional<JSValue> paramsObj) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        bromesh::MeshletParams p;
        if (paramsObj && JS_IsObject(*paramsObj)) {
            JSValue v;
            v = JS_GetPropertyStr(ctx, *paramsObj, "maxVertices");
            if (!JS_IsUndefined(v)) { int32_t x; JS_ToInt32(ctx, &x, v); p.maxVertices = (size_t)x; }
            JS_FreeValue(ctx, v);
            v = JS_GetPropertyStr(ctx, *paramsObj, "maxTriangles");
            if (!JS_IsUndefined(v)) { int32_t x; JS_ToInt32(ctx, &x, v); p.maxTriangles = (size_t)x; }
            JS_FreeValue(ctx, v);
            v = JS_GetPropertyStr(ctx, *paramsObj, "coneWeight");
            if (!JS_IsUndefined(v)) { double x; JS_ToFloat64(ctx, &x, v); p.coneWeight = (float)x; }
            JS_FreeValue(ctx, v);
        }
        auto ml = bromesh::buildMeshlets(*w->data, p);
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < ml.size(); i++) {
            JSValue o = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, o, "vertices",  makeUint32Array(ctx, ml[i].vertices));
            std::vector<uint8_t> triBuf(ml[i].triangles.begin(), ml[i].triangles.end());
            JS_SetPropertyStr(ctx, o, "triangles", makeUint8Array(ctx, triBuf));
            JSValue b = JS_NewObject(ctx);
            JSValue cen = JS_NewArray(ctx);
            JSValue apex = JS_NewArray(ctx);
            JSValue axis = JS_NewArray(ctx);
            for (int k = 0; k < 3; k++) {
                JS_SetPropertyUint32(ctx, cen,  k, JS_NewFloat64(ctx, ml[i].bounds.center[k]));
                JS_SetPropertyUint32(ctx, apex, k, JS_NewFloat64(ctx, ml[i].bounds.coneApex[k]));
                JS_SetPropertyUint32(ctx, axis, k, JS_NewFloat64(ctx, ml[i].bounds.coneAxis[k]));
            }
            JS_SetPropertyStr(ctx, b, "center",     cen);
            JS_SetPropertyStr(ctx, b, "radius",     JS_NewFloat64(ctx, ml[i].bounds.radius));
            JS_SetPropertyStr(ctx, b, "coneApex",   apex);
            JS_SetPropertyStr(ctx, b, "coneAxis",   axis);
            JS_SetPropertyStr(ctx, b, "coneCutoff", JS_NewFloat64(ctx, ml[i].bounds.coneCutoff));
            JS_SetPropertyStr(ctx, o, "bounds",    b);
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
        }
        return arr;
    })

    // ── Encode (for streaming) ─────────────────────────────────────────
    .method("encode", [](MW* w, JSContext* ctx) -> JSValue {
        if (!w->data) return JS_UNDEFINED;
        auto e = bromesh::encodeMesh(*w->data);
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "vertexData",  makeUint8Array(ctx, e.vertexData));
        JS_SetPropertyStr(ctx, o, "indexData",   makeUint8Array(ctx, e.indexData));
        JS_SetPropertyStr(ctx, o, "vertexCount", JS_NewInt64(ctx, (int64_t)e.vertexCount));
        JS_SetPropertyStr(ctx, o, "vertexSize",  JS_NewInt64(ctx, (int64_t)e.vertexSize));
        JS_SetPropertyStr(ctx, o, "indexCount",  JS_NewInt64(ctx, (int64_t)e.indexCount));
        JS_SetPropertyStr(ctx, o, "hasNormals",  JS_NewBool(ctx, w->data->hasNormals()));
        JS_SetPropertyStr(ctx, o, "hasUVs",      JS_NewBool(ctx, w->data->hasUVs()));
        JS_SetPropertyStr(ctx, o, "hasColors",   JS_NewBool(ctx, w->data->hasColors()));
        return o;
    })

    // ── Skinning / morph (rigging) ─────────────────────────────────────
    .method("applySkinning", [](MW* w, JSContext* ctx, JSValue skinVal, JSValue matricesVal) -> JSValue {
        if (!w->data) return JS_ThrowTypeError(ctx, "neutered Mesh");
        auto* skin = RiggingBindings::getSkinData(ctx, skinVal);
        if (!skin) return JS_ThrowTypeError(ctx, "first argument must be a SkinData");
        std::vector<float> mats;
        if (!readFloatArrayVal(ctx, matricesVal, mats))
            return JS_ThrowTypeError(ctx, "second argument must be a Float32Array of pose matrices");
        size_t needed = skin->boneCount * 16;
        if (mats.size() < needed)
            return JS_ThrowRangeError(ctx, "pose matrices length %zu < boneCount*16 (%zu)",
                                      mats.size(), needed);
        bromesh::applySkinning(*w->data, *skin, mats.data());
        return JS_UNDEFINED;
    })
    .method("applyMorphTarget", [](MW* w, JSContext* ctx, JSValue morph, double weight) -> JSValue {
        if (!w->data) return JS_ThrowTypeError(ctx, "neutered Mesh");
        if (!JS_IsObject(morph)) return JS_ThrowTypeError(ctx, "morph target must be an object");
        bromesh::MorphTarget mt;
        JSValue v;

        v = JS_GetPropertyStr(ctx, morph, "name");
        if (JS_IsString(v)) {
            const char* s = JS_ToCString(ctx, v);
            if (s) { mt.name = s; JS_FreeCString(ctx, s); }
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, morph, "deltaPositions");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) readFloatArrayVal(ctx, v, mt.deltaPositions);
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, morph, "deltaNormals");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) readFloatArrayVal(ctx, v, mt.deltaNormals);
        JS_FreeValue(ctx, v);

        bromesh::applyMorphTarget(*w->data, mt, (float)weight);
        return JS_UNDEFINED;
    })

    // ── Static: Primitives ──────────────────────────────────────────────
    .static_method("box", [](JSContext* ctx, std::optional<double> hw, std::optional<double> hh,
                              std::optional<double> hd) -> JSValue {
        return wrapMesh(ctx, bromesh::box((float)hw.value_or(0.5), (float)hh.value_or(0.5), (float)hd.value_or(0.5)));
    })
    .static_method("sphere", [](JSContext* ctx, std::optional<double> r, std::optional<int> seg,
                                 std::optional<int> rings) -> JSValue {
        return wrapMesh(ctx, bromesh::sphere((float)r.value_or(0.5), seg.value_or(16), rings.value_or(12)));
    })
    .static_method("cylinder", [](JSContext* ctx, std::optional<double> r, std::optional<double> hh,
                                   std::optional<int> seg) -> JSValue {
        return wrapMesh(ctx, bromesh::cylinder((float)r.value_or(0.5), (float)hh.value_or(0.5), seg.value_or(16)));
    })
    .static_method("capsule", [](JSContext* ctx, std::optional<double> r, std::optional<double> hh,
                                  std::optional<int> seg, std::optional<int> rings) -> JSValue {
        return wrapMesh(ctx, bromesh::capsule((float)r.value_or(0.5), (float)hh.value_or(0.5), seg.value_or(16), rings.value_or(8)));
    })
    .static_method("plane", [](JSContext* ctx, std::optional<double> hw, std::optional<double> hd,
                                std::optional<int> sx, std::optional<int> sz) -> JSValue {
        return wrapMesh(ctx, bromesh::plane((float)hw.value_or(5.0), (float)hd.value_or(5.0), sx.value_or(1), sz.value_or(1)));
    })
    .static_method("torus", [](JSContext* ctx, std::optional<double> major, std::optional<double> minor,
                                std::optional<int> majSeg, std::optional<int> minSeg) -> JSValue {
        return wrapMesh(ctx, bromesh::torus((float)major.value_or(1.0), (float)minor.value_or(0.3), majSeg.value_or(24), minSeg.value_or(12)));
    })
    .static_raw("heightmapGrid", js_heightmapGrid, 3)

    // ── Static: par_primitives (procedural / Platonic) ─────────────────
    .static_method("geodesicSphere", [](JSContext* ctx, std::optional<double> r, std::optional<int> nsub) -> JSValue {
        return wrapMesh(ctx, bromesh::geodesicSphere((float)r.value_or(0.5), nsub.value_or(2)));
    })
    .static_method("icosahedron",  [](JSContext* ctx) -> JSValue { return wrapMesh(ctx, bromesh::icosahedron()); })
    .static_method("dodecahedron", [](JSContext* ctx) -> JSValue { return wrapMesh(ctx, bromesh::dodecahedron()); })
    .static_method("octahedron",   [](JSContext* ctx) -> JSValue { return wrapMesh(ctx, bromesh::octahedron()); })
    .static_method("tetrahedron",  [](JSContext* ctx) -> JSValue { return wrapMesh(ctx, bromesh::tetrahedron()); })
    .static_method("cone", [](JSContext* ctx, std::optional<double> r, std::optional<double> h,
                               std::optional<int> slices, std::optional<int> stacks) -> JSValue {
        return wrapMesh(ctx, bromesh::cone((float)r.value_or(0.5), (float)h.value_or(1.0),
                                           slices.value_or(16), stacks.value_or(4)));
    })
    .static_method("disc", [](JSContext* ctx, std::optional<double> r, std::optional<int> slices) -> JSValue {
        return wrapMesh(ctx, bromesh::disc((float)r.value_or(0.5), slices.value_or(16)));
    })
    .static_method("rock", [](JSContext* ctx, std::optional<double> r, std::optional<int> seed, std::optional<int> nsub) -> JSValue {
        return wrapMesh(ctx, bromesh::rock((float)r.value_or(0.5), seed.value_or(42), nsub.value_or(2)));
    })
    .static_method("trefoilKnot", [](JSContext* ctx, std::optional<double> r, std::optional<int> slices, std::optional<int> stacks) -> JSValue {
        return wrapMesh(ctx, bromesh::trefoilKnot((float)r.value_or(1.0), slices.value_or(64), stacks.value_or(16)));
    })
    .static_method("kleinBottle", [](JSContext* ctx, std::optional<int> slices, std::optional<int> stacks) -> JSValue {
        return wrapMesh(ctx, bromesh::kleinBottle(slices.value_or(32), stacks.value_or(16)));
    })

    // ── Static: CSG ─────────────────────────────────────────────────────
    .static_method("union", [](JSContext* ctx, JSValue a, JSValue b) -> JSValue {
        auto* ma = qjsbind::unwrap<MW>(ctx, a);
        auto* mb = qjsbind::unwrap<MW>(ctx, b);
        if (!ma || !ma->data || !mb || !mb->data) return JS_ThrowTypeError(ctx, "arguments must be Mesh instances");
        auto r = bromesh::booleanUnion(*ma->data, *mb->data);
        return wrapMesh(ctx, bromesh::computeCreaseNormals(r));
    })
    .static_method("subtract", [](JSContext* ctx, JSValue a, JSValue b) -> JSValue {
        auto* ma = qjsbind::unwrap<MW>(ctx, a);
        auto* mb = qjsbind::unwrap<MW>(ctx, b);
        if (!ma || !ma->data || !mb || !mb->data) return JS_ThrowTypeError(ctx, "arguments must be Mesh instances");
        auto r = bromesh::booleanDifference(*ma->data, *mb->data);
        return wrapMesh(ctx, bromesh::computeCreaseNormals(r));
    })
    .static_method("intersect", [](JSContext* ctx, JSValue a, JSValue b) -> JSValue {
        auto* ma = qjsbind::unwrap<MW>(ctx, a);
        auto* mb = qjsbind::unwrap<MW>(ctx, b);
        if (!ma || !ma->data || !mb || !mb->data) return JS_ThrowTypeError(ctx, "arguments must be Mesh instances");
        auto r = bromesh::booleanIntersection(*ma->data, *mb->data);
        return wrapMesh(ctx, bromesh::computeCreaseNormals(r));
    })
    .static_raw("splitByPlane", js_splitByPlane, 5)
    .static_method("merge", [](JSContext* ctx, JSValue meshArr) -> JSValue {
        if (!JS_IsArray(meshArr)) return JS_ThrowTypeError(ctx, "merge requires an array of Mesh instances");
        JSValue lenVal = JS_GetPropertyStr(ctx, meshArr, "length");
        int32_t len = 0; JS_ToInt32(ctx, &len, lenVal); JS_FreeValue(ctx, lenVal);
        bromesh::MeshData result;
        for (int32_t i = 0; i < len; i++) {
            JSValue elem = JS_GetPropertyUint32(ctx, meshArr, (uint32_t)i);
            auto* w = qjsbind::unwrap<MW>(ctx, elem);
            if (w && w->data) {
                uint32_t base = (uint32_t)(result.positions.size() / 3);
                result.positions.insert(result.positions.end(), w->data->positions.begin(), w->data->positions.end());
                result.normals.insert(result.normals.end(), w->data->normals.begin(), w->data->normals.end());
                result.uvs.insert(result.uvs.end(), w->data->uvs.begin(), w->data->uvs.end());
                result.colors.insert(result.colors.end(), w->data->colors.begin(), w->data->colors.end());
                for (auto idx : w->data->indices) result.indices.push_back(idx + base);
            }
            JS_FreeValue(ctx, elem);
        }
        return wrapMesh(ctx, std::move(result));
    })

    // ── Static: Isosurface ──────────────────────────────────────────────
    .static_raw("marchingCubes", js_marchingCubes, 4)
    .static_raw("dualContour",   js_dualContour, 4)
    .static_raw("surfaceNets",   js_surfaceNets, 4)
    .static_raw("transvoxel",    js_transvoxel, 4)
    .static_raw("greedyMesh",    js_greedyMesh, 4)

    // ── Static: I/O (load) ──────────────────────────────────────────────
    .static_method("loadGLTF", [](JSContext* ctx, std::string path) -> JSValue {
        auto scene = bromesh::loadGLTF(path);
        JSValue obj = JS_NewObject(ctx);
        JSValue meshArr = JS_NewArray(ctx);
        for (size_t i = 0; i < scene.meshes.size(); i++)
            JS_SetPropertyUint32(ctx, meshArr, (uint32_t)i, wrapMesh(ctx, std::move(scene.meshes[i])));
        JS_SetPropertyStr(ctx, obj, "meshes", meshArr);
        return obj;
    })
    .static_method("loadOBJ", [](JSContext* ctx, std::string path) -> JSValue {
        return wrapMesh(ctx, bromesh::loadOBJ(path));
    })
    .static_method("loadFBX", [](JSContext* ctx, std::string path) -> JSValue {
        auto meshes = bromesh::loadFBX(path);
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < meshes.size(); i++)
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, wrapMesh(ctx, std::move(meshes[i])));
        return arr;
    })
    .static_method("loadPLY", [](JSContext* ctx, std::string path) -> JSValue {
        return wrapMesh(ctx, bromesh::loadPLY(path));
    })
    .static_method("loadSTL", [](JSContext* ctx, std::string path) -> JSValue {
        return wrapMesh(ctx, bromesh::loadSTL(path));
    })
    .static_method("loadVOX", [](JSContext* ctx, std::string path) -> JSValue {
        auto vox = bromesh::loadVOX(path);
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "sizeX", JS_NewInt32(ctx, vox.sizeX));
        JS_SetPropertyStr(ctx, obj, "sizeY", JS_NewInt32(ctx, vox.sizeY));
        JS_SetPropertyStr(ctx, obj, "sizeZ", JS_NewInt32(ctx, vox.sizeZ));
        {
            JSValue abuf = JS_NewArrayBufferCopy(ctx, vox.voxels.data(), vox.voxels.size());
            JSValue args[3] = { abuf, JS_UNDEFINED, JS_UNDEFINED };
            JS_SetPropertyStr(ctx, obj, "voxels", JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_UINT8));
            JS_FreeValue(ctx, abuf);
        }
        {
            size_t bytes = 256 * 4 * sizeof(float);
            JSValue abuf = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(vox.palette), bytes);
            JSValue args[3] = { abuf, JS_UNDEFINED, JS_UNDEFINED };
            JS_SetPropertyStr(ctx, obj, "palette", JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_FLOAT32));
            JS_FreeValue(ctx, abuf);
        }
        return obj;
    })

    // ── Static: Reconstruction ──────────────────────────────────────────
    .static_raw("reconstruct", js_reconstruct, 1)

    // ── Static: Encoding / stripification ──────────────────────────────
    .static_raw("decode", js_decode, 1)
    .static_method("stripify", [](JSContext* ctx, JSValue indicesVal, int vertexCount, std::optional<int> restartIdx) -> JSValue {
        std::vector<uint32_t> idx;
        if (!readUint32ArrayVal(ctx, indicesVal, idx)) return JS_ThrowTypeError(ctx, "indices must be Uint32Array");
        auto strip = bromesh::stripify(idx, (size_t)vertexCount, (uint32_t)restartIdx.value_or((int)0xFFFFFFFF));
        return makeUint32Array(ctx, strip);
    })
    .static_method("unstripify", [](JSContext* ctx, JSValue stripVal, std::optional<int> restartIdx) -> JSValue {
        std::vector<uint32_t> strip;
        if (!readUint32ArrayVal(ctx, stripVal, strip)) return JS_ThrowTypeError(ctx, "strip must be Uint32Array");
        auto list = bromesh::unstripify(strip, (uint32_t)restartIdx.value_or((int)0xFFFFFFFF));
        return makeUint32Array(ctx, list);
    })
    ; // end of Class<MW>

    // =======================================================================
    // MeshBVH — acceleration structure for repeated ray queries
    // =======================================================================
    qjsbind::Class<BVW>(ctx, "MeshBVH")
    .constructor([](JSContext* ctx, int argc, JSValueConst* argv) -> BVW* {
        if (argc < 1) return new BVW{std::make_unique<bromesh::MeshBVH>()};
        auto* mw = qjsbind::unwrap<MW>(ctx, argv[0]);
        if (!mw || !mw->data) return new BVW{std::make_unique<bromesh::MeshBVH>()};
        int leafSize = 8;
        if (argc > 1) { int32_t x; JS_ToInt32(ctx, &x, argv[1]); leafSize = x; }
        auto bvh = bromesh::MeshBVH::build(*mw->data, leafSize);
        return new BVW{std::make_unique<bromesh::MeshBVH>(std::move(bvh))};
    })
    .get("empty",         [](BVW* w) { return !w->bvh || w->bvh->empty(); })
    .get("nodeCount",     [](BVW* w) { return (int)(w->bvh ? w->bvh->nodeCount() : 0); })
    .get("triangleCount", [](BVW* w) { return (int)(w->bvh ? w->bvh->triangleCount() : 0); })
    .method("bounds", [](BVW* w, JSContext* ctx) -> JSValue {
        return w->bvh ? makeBBox(ctx, w->bvh->bounds()) : JS_UNDEFINED;
    })
    .method("raycast", [](BVW* w, JSContext* ctx, JSValue meshVal, JSValue origin, JSValue direction,
                           std::optional<double> maxDist) -> JSValue {
        if (!w->bvh) return JS_UNDEFINED;
        auto* mw = qjsbind::unwrap<MW>(ctx, meshVal);
        if (!mw || !mw->data) return JS_ThrowTypeError(ctx, "first argument must be the source Mesh");
        float o[3], d[3];
        readVec3(ctx, origin, o);
        readVec3(ctx, direction, d);
        return makeRayHit(ctx, w->bvh->raycast(*mw->data, o, d, (float)maxDist.value_or(0.0)));
    })
    .method("raycastTest", [](BVW* w, JSContext* ctx, JSValue meshVal, JSValue origin, JSValue direction,
                               std::optional<double> maxDist) -> JSValue {
        if (!w->bvh) return JS_FALSE;
        auto* mw = qjsbind::unwrap<MW>(ctx, meshVal);
        if (!mw || !mw->data) return JS_ThrowTypeError(ctx, "first argument must be the source Mesh");
        float o[3], d[3];
        readVec3(ctx, origin, o);
        readVec3(ctx, direction, d);
        return JS_NewBool(ctx, w->bvh->raycastTest(*mw->data, o, d, (float)maxDist.value_or(0.0)));
    })
    ;

    // =======================================================================
    // ProgressiveMesh — LOD streaming via edge-collapse records
    // =======================================================================
    qjsbind::Class<PMW>(ctx, "ProgressiveMesh")
    .constructor([](JSContext* ctx, int argc, JSValueConst* argv) -> PMW* {
        if (argc < 1) return new PMW{std::make_unique<bromesh::ProgressiveMesh>()};
        auto* mw = qjsbind::unwrap<MW>(ctx, argv[0]);
        if (!mw || !mw->data) return new PMW{std::make_unique<bromesh::ProgressiveMesh>()};
        auto pm = bromesh::buildProgressiveMesh(*mw->data);
        return new PMW{std::make_unique<bromesh::ProgressiveMesh>(std::move(pm))};
    })
    .get("maxTriangles", [](PMW* w) { return (int)(w->pm ? w->pm->maxTriangles() : 0); })
    .get("minTriangles", [](PMW* w) { return (int)(w->pm ? w->pm->minTriangles() : 0); })
    .get("collapseCount",[](PMW* w) { return (int)(w->pm ? w->pm->collapses.size() : 0); })
    .method("atTriangleCount", [](PMW* w, JSContext* ctx, int n) -> JSValue {
        if (!w->pm) return JS_UNDEFINED;
        return wrapMesh(ctx, bromesh::progressiveMeshAtTriangleCount(*w->pm, (size_t)n));
    })
    .method("atRatio", [](PMW* w, JSContext* ctx, double r) -> JSValue {
        if (!w->pm) return JS_UNDEFINED;
        return wrapMesh(ctx, bromesh::progressiveMeshAtRatio(*w->pm, (float)r));
    })
    .method("serialize", [](PMW* w, JSContext* ctx) -> JSValue {
        if (!w->pm) return JS_UNDEFINED;
        return makeUint8Array(ctx, bromesh::serializeProgressiveMesh(*w->pm));
    })
    .static_method("deserialize", [](JSContext* ctx, JSValue bytesVal) -> JSValue {
        std::vector<uint8_t> bytes;
        if (!readUint8ArrayVal(ctx, bytesVal, bytes)) return JS_ThrowTypeError(ctx, "expected Uint8Array");
        auto pm = bromesh::deserializeProgressiveMesh(bytes.data(), bytes.size());
        return qjsbind::wrap<PMW>(ctx, new PMW{std::make_unique<bromesh::ProgressiveMesh>(std::move(pm))});
    })
    ;
}

// ---------------------------------------------------------------------------
// Cleanup — no-op with qjsbind (destructor handles lifecycle)
// ---------------------------------------------------------------------------

void MeshBindings::cleanup(JSContext*) {}

// ---------------------------------------------------------------------------
// Public API — used by worker thread transfer and scene_bindings
// ---------------------------------------------------------------------------

bromesh::MeshData* MeshBindings::getMeshData(JSContext* ctx, JSValueConst val) {
    auto* w = qjsbind::unwrap<MW>(ctx, val);
    return w ? w->data.get() : nullptr;
}

std::unique_ptr<bromesh::MeshData> MeshBindings::takeMeshData(JSContext* ctx, JSValueConst val) {
    auto* w = qjsbind::unwrap<MW>(ctx, val);
    if (!w) return nullptr;
    return std::move(w->data);
}

JSValue MeshBindings::wrapMeshData(JSContext* ctx, std::unique_ptr<bromesh::MeshData> data) {
    if (!data) return JS_ThrowTypeError(ctx, "wrapMeshData: null MeshData");
    return qjsbind::wrap<MW>(ctx, new MW{std::move(data)});
}

JSClassID MeshBindings::classId() {
    return qjsbind::class_id<MW>();
}

} // namespace bro::js
