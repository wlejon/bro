#include "js/mesh_bindings.h"

#include <bromesh/mesh_data.h>
#include <bromesh/primitives/primitives.h>
#include <bromesh/analysis/bbox.h>
#include <bromesh/analysis/raycast.h>
#include <bromesh/analysis/intersect.h>
#include <bromesh/manipulation/normals.h>
#include <bromesh/manipulation/simplify.h>
#include <bromesh/manipulation/subdivide.h>
#include <bromesh/manipulation/smooth.h>
#include <bromesh/optimization/optimize.h>
#include <bromesh/isosurface/marching_cubes.h>
#include <bromesh/isosurface/dual_contouring.h>
#include <bromesh/csg/boolean.h>
#include <bromesh/uv/unwrap.h>
#include <bromesh/uv/projection.h>
#include <bromesh/voxel/greedy_mesh.h>
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
// Class ID
// ---------------------------------------------------------------------------

static JSClassID js_mesh_class_id = 0;

// ---------------------------------------------------------------------------
// Opaque wrapper
// ---------------------------------------------------------------------------

struct MeshWrapper {
    std::unique_ptr<bromesh::MeshData> data;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static MeshWrapper* getMW(JSValueConst val) {
    return static_cast<MeshWrapper*>(JS_GetOpaque(val, js_mesh_class_id));
}

static bromesh::MeshData* getMD(JSValueConst val) {
    auto* w = getMW(val);
    return w ? w->data.get() : nullptr;
}

/// Create a new JS Mesh object wrapping the given MeshData.
static JSValue wrapMesh(JSContext* ctx, bromesh::MeshData&& data);

/// Read a typed array (Float32Array or Uint8Array etc.) from a property into a float vector.
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
    size_t count = byteLen / sizeof(float);
    out.assign(data, data + count);
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
    size_t count = byteLen / sizeof(uint32_t);
    out.assign(data, data + count);
    JS_FreeValue(ctx, v);
    return true;
}

/// Read a Float32Array directly from a JS value (not a property).
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

/// Create a Float32Array from a vector<float>.
static JSValue makeFloat32Array(JSContext* ctx, const std::vector<float>& vec) {
    size_t bytes = vec.size() * sizeof(float);
    JSValue abuf = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(vec.data()), bytes);
    JSValue arr = JS_NewTypedArray(ctx, 1, &abuf, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(ctx, abuf);
    return arr;
}

/// Create a Uint32Array from a vector<uint32_t>.
static JSValue makeUint32Array(JSContext* ctx, const std::vector<uint32_t>& vec) {
    size_t bytes = vec.size() * sizeof(uint32_t);
    JSValue abuf = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(vec.data()), bytes);
    JSValue arr = JS_NewTypedArray(ctx, 1, &abuf, JS_TYPED_ARRAY_UINT32);
    JS_FreeValue(ctx, abuf);
    return arr;
}

static double optNum(JSContext* ctx, JSValueConst* argv, int argc, int idx, double def) {
    if (idx >= argc || JS_IsUndefined(argv[idx])) return def;
    double v = def;
    JS_ToFloat64(ctx, &v, argv[idx]);
    return v;
}

static int optInt(JSContext* ctx, JSValueConst* argv, int argc, int idx, int def) {
    if (idx >= argc || JS_IsUndefined(argv[idx])) return def;
    int32_t v = def;
    JS_ToInt32(ctx, &v, argv[idx]);
    return v;
}

static std::string toStr(JSContext* ctx, JSValueConst val) {
    const char* s = JS_ToCString(ctx, val);
    std::string r = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    return r;
}

// ---------------------------------------------------------------------------
// Finalizer
// ---------------------------------------------------------------------------

static void js_mesh_finalizer(JSRuntime*, JSValue val) {
    delete getMW(val);
}

static JSClassDef js_mesh_class = {
    "Mesh", js_mesh_finalizer, nullptr, nullptr, nullptr
};

// ---------------------------------------------------------------------------
// Constructor: new Mesh() or new Mesh({ positions, normals, uvs, colors, indices })
// ---------------------------------------------------------------------------

static JSValue js_mesh_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv) {
    auto md = std::make_unique<bromesh::MeshData>();

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];
        readFloatArray(ctx, opts, "positions", md->positions);
        readFloatArray(ctx, opts, "normals", md->normals);
        readFloatArray(ctx, opts, "uvs", md->uvs);
        readFloatArray(ctx, opts, "colors", md->colors);
        readUint32Array(ctx, opts, "indices", md->indices);
    }

    return wrapMesh(ctx, std::move(*md));
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

static JSValue js_mesh_get_positions(JSContext* ctx, JSValueConst this_val) {
    auto* md = getMD(this_val);
    return md ? makeFloat32Array(ctx, md->positions) : JS_UNDEFINED;
}
static JSValue js_mesh_set_positions(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* md = getMD(this_val);
    if (md) readFloatArrayVal(ctx, val, md->positions);
    return JS_UNDEFINED;
}

static JSValue js_mesh_get_normals(JSContext* ctx, JSValueConst this_val) {
    auto* md = getMD(this_val);
    return md ? makeFloat32Array(ctx, md->normals) : JS_UNDEFINED;
}
static JSValue js_mesh_set_normals(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* md = getMD(this_val);
    if (md) readFloatArrayVal(ctx, val, md->normals);
    return JS_UNDEFINED;
}

static JSValue js_mesh_get_uvs(JSContext* ctx, JSValueConst this_val) {
    auto* md = getMD(this_val);
    return md ? makeFloat32Array(ctx, md->uvs) : JS_UNDEFINED;
}
static JSValue js_mesh_set_uvs(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* md = getMD(this_val);
    if (md) readFloatArrayVal(ctx, val, md->uvs);
    return JS_UNDEFINED;
}

static JSValue js_mesh_get_colors(JSContext* ctx, JSValueConst this_val) {
    auto* md = getMD(this_val);
    return md ? makeFloat32Array(ctx, md->colors) : JS_UNDEFINED;
}
static JSValue js_mesh_set_colors(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* md = getMD(this_val);
    if (md) readFloatArrayVal(ctx, val, md->colors);
    return JS_UNDEFINED;
}

static JSValue js_mesh_get_indices(JSContext* ctx, JSValueConst this_val) {
    auto* md = getMD(this_val);
    return md ? makeUint32Array(ctx, md->indices) : JS_UNDEFINED;
}
static JSValue js_mesh_set_indices(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* md = getMD(this_val);
    if (md) readUint32ArrayVal(ctx, val, md->indices);
    return JS_UNDEFINED;
}

static JSValue js_mesh_get_vertexCount(JSContext* ctx, JSValueConst this_val) {
    auto* md = getMD(this_val);
    return md ? JS_NewInt32(ctx, (int32_t)md->vertexCount()) : JS_UNDEFINED;
}
static JSValue js_mesh_get_triangleCount(JSContext* ctx, JSValueConst this_val) {
    auto* md = getMD(this_val);
    return md ? JS_NewInt32(ctx, (int32_t)md->triangleCount()) : JS_UNDEFINED;
}
static JSValue js_mesh_get_hasNormals(JSContext* ctx, JSValueConst this_val) {
    auto* md = getMD(this_val);
    return md ? JS_NewBool(ctx, md->hasNormals()) : JS_UNDEFINED;
}
static JSValue js_mesh_get_hasUVs(JSContext* ctx, JSValueConst this_val) {
    auto* md = getMD(this_val);
    return md ? JS_NewBool(ctx, md->hasUVs()) : JS_UNDEFINED;
}
static JSValue js_mesh_get_hasColors(JSContext* ctx, JSValueConst this_val) {
    auto* md = getMD(this_val);
    return md ? JS_NewBool(ctx, md->hasColors()) : JS_UNDEFINED;
}
static JSValue js_mesh_get_empty(JSContext* ctx, JSValueConst this_val) {
    auto* md = getMD(this_val);
    return md ? JS_NewBool(ctx, md->empty()) : JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Instance methods — clone
// ---------------------------------------------------------------------------

static JSValue js_mesh_clone(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* md = getMD(this_val);
    if (!md) return JS_UNDEFINED;
    return wrapMesh(ctx, bromesh::MeshData(*md));
}

// ---------------------------------------------------------------------------
// Instance methods — transforms
// ---------------------------------------------------------------------------

static JSValue js_mesh_translate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* md = getMD(this_val);
    if (!md || argc < 3) return JS_UNDEFINED;
    float dx = (float)optNum(ctx, argv, argc, 0, 0);
    float dy = (float)optNum(ctx, argv, argc, 1, 0);
    float dz = (float)optNum(ctx, argv, argc, 2, 0);
    for (size_t i = 0; i < md->positions.size(); i += 3) {
        md->positions[i]     += dx;
        md->positions[i + 1] += dy;
        md->positions[i + 2] += dz;
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue js_mesh_scale(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* md = getMD(this_val);
    if (!md || argc < 1) return JS_UNDEFINED;
    float sx = (float)optNum(ctx, argv, argc, 0, 1);
    float sy = (argc >= 3) ? (float)optNum(ctx, argv, argc, 1, 1) : sx;
    float sz = (argc >= 3) ? (float)optNum(ctx, argv, argc, 2, 1) : sx;
    for (size_t i = 0; i < md->positions.size(); i += 3) {
        md->positions[i]     *= sx;
        md->positions[i + 1] *= sy;
        md->positions[i + 2] *= sz;
    }
    // If non-uniform scale, normals need recalculation
    if (sx != sy || sy != sz) bromesh::computeNormals(*md);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_mesh_rotate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* md = getMD(this_val);
    if (!md || argc < 4) return JS_UNDEFINED;
    float ax = (float)optNum(ctx, argv, argc, 0, 0);
    float ay = (float)optNum(ctx, argv, argc, 1, 0);
    float az = (float)optNum(ctx, argv, argc, 2, 0);
    float angle = (float)optNum(ctx, argv, argc, 3, 0);

    // Normalize axis
    float len = std::sqrt(ax*ax + ay*ay + az*az);
    if (len < 1e-8f) return JS_DupValue(ctx, this_val);
    ax /= len; ay /= len; az /= len;

    float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    // Rotation matrix
    float m00 = t*ax*ax + c,     m01 = t*ax*ay - s*az, m02 = t*ax*az + s*ay;
    float m10 = t*ax*ay + s*az,  m11 = t*ay*ay + c,    m12 = t*ay*az - s*ax;
    float m20 = t*ax*az - s*ay,  m21 = t*ay*az + s*ax, m22 = t*az*az + c;

    auto rot = [&](std::vector<float>& v) {
        for (size_t i = 0; i < v.size(); i += 3) {
            float x = v[i], y = v[i+1], z = v[i+2];
            v[i]   = m00*x + m01*y + m02*z;
            v[i+1] = m10*x + m11*y + m12*z;
            v[i+2] = m20*x + m21*y + m22*z;
        }
    };
    rot(md->positions);
    if (md->hasNormals()) rot(md->normals);

    return JS_DupValue(ctx, this_val);
}

static JSValue js_mesh_center(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* md = getMD(this_val);
    if (!md || md->positions.empty()) return JS_DupValue(ctx, this_val);
    auto bbox = bromesh::computeBBox(*md);
    float cx = bbox.centerX(), cy = bbox.centerY(), cz = bbox.centerZ();
    for (size_t i = 0; i < md->positions.size(); i += 3) {
        md->positions[i]     -= cx;
        md->positions[i + 1] -= cy;
        md->positions[i + 2] -= cz;
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue js_mesh_mirror(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* md = getMD(this_val);
    if (!md || argc < 1) return JS_UNDEFINED;
    int axis = optInt(ctx, argv, argc, 0, 0); // 0=X, 1=Y, 2=Z
    if (axis < 0 || axis > 2) return JS_DupValue(ctx, this_val);
    for (size_t i = 0; i < md->positions.size(); i += 3) {
        md->positions[i + axis] = -md->positions[i + axis];
    }
    if (md->hasNormals()) {
        for (size_t i = 0; i < md->normals.size(); i += 3) {
            md->normals[i + axis] = -md->normals[i + axis];
        }
    }
    // Flip winding order
    for (size_t i = 0; i + 2 < md->indices.size(); i += 3) {
        std::swap(md->indices[i + 1], md->indices[i + 2]);
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue js_mesh_transform(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* md = getMD(this_val);
    if (!md || argc < 1) return JS_UNDEFINED;
    std::vector<float> mat;
    if (!readFloatArrayVal(ctx, argv[0], mat) || mat.size() < 16) return JS_UNDEFINED;
    const float* m = mat.data(); // column-major 4x4
    for (size_t i = 0; i < md->positions.size(); i += 3) {
        float x = md->positions[i], y = md->positions[i+1], z = md->positions[i+2];
        md->positions[i]   = m[0]*x + m[4]*y + m[8]*z  + m[12];
        md->positions[i+1] = m[1]*x + m[5]*y + m[9]*z  + m[13];
        md->positions[i+2] = m[2]*x + m[6]*y + m[10]*z + m[14];
    }
    // Transform normals by inverse-transpose of upper-3x3 (for uniform scale, same as upper-3x3)
    if (md->hasNormals()) {
        for (size_t i = 0; i < md->normals.size(); i += 3) {
            float x = md->normals[i], y = md->normals[i+1], z = md->normals[i+2];
            md->normals[i]   = m[0]*x + m[4]*y + m[8]*z;
            md->normals[i+1] = m[1]*x + m[5]*y + m[9]*z;
            md->normals[i+2] = m[2]*x + m[6]*y + m[10]*z;
            // Renormalize
            float len = std::sqrt(md->normals[i]*md->normals[i] +
                                  md->normals[i+1]*md->normals[i+1] +
                                  md->normals[i+2]*md->normals[i+2]);
            if (len > 1e-8f) {
                md->normals[i] /= len;
                md->normals[i+1] /= len;
                md->normals[i+2] /= len;
            }
        }
    }
    return JS_DupValue(ctx, this_val);
}

// ---------------------------------------------------------------------------
// Instance methods — normals
// ---------------------------------------------------------------------------

static JSValue js_mesh_computeNormals(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* md = getMD(this_val);
    if (md) bromesh::computeNormals(*md);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_mesh_computeFlatNormals(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* md = getMD(this_val);
    if (!md) return JS_UNDEFINED;
    return wrapMesh(ctx, bromesh::computeFlatNormals(*md));
}

static JSValue js_mesh_computeTangents(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* md = getMD(this_val);
    if (!md) return JS_UNDEFINED;
    auto tangents = bromesh::computeTangents(*md);
    return makeFloat32Array(ctx, tangents);
}

// ---------------------------------------------------------------------------
// Instance methods — simplification
// ---------------------------------------------------------------------------

static JSValue js_mesh_simplify(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = getMW(this_val);
    if (!w || argc < 1) return JS_UNDEFINED;
    float ratio = (float)optNum(ctx, argv, argc, 0, 0.5);
    float error = (float)optNum(ctx, argv, argc, 1, 0.01);
    *w->data = bromesh::simplify(*w->data, ratio, error);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_mesh_simplifyWithAttributes(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = getMW(this_val);
    if (!w || argc < 1) return JS_UNDEFINED;
    float ratio = (float)optNum(ctx, argv, argc, 0, 0.5);
    float error = (float)optNum(ctx, argv, argc, 1, 0.01);
    float uvW = (float)optNum(ctx, argv, argc, 2, 1.0);
    float nW = (float)optNum(ctx, argv, argc, 3, 0.5);
    *w->data = bromesh::simplifyWithAttributes(*w->data, ratio, error, uvW, nW);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_mesh_simplifyToTriangleCount(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = getMW(this_val);
    if (!w || argc < 1) return JS_UNDEFINED;
    int target = optInt(ctx, argv, argc, 0, 100);
    float error = (float)optNum(ctx, argv, argc, 1, 0.01);
    *w->data = bromesh::simplifyToTriangleCount(*w->data, (size_t)target, error);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_mesh_generateLODChain(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* md = getMD(this_val);
    if (!md || argc < 1) return JS_UNDEFINED;
    std::vector<float> ratios;
    if (!readFloatArrayVal(ctx, argv[0], ratios) || ratios.empty()) return JS_UNDEFINED;
    auto lods = bromesh::generateLODChain(*md, ratios.data(), (int)ratios.size());
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < lods.size(); i++) {
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, wrapMesh(ctx, std::move(lods[i])));
    }
    return arr;
}

// ---------------------------------------------------------------------------
// Instance methods — subdivision
// ---------------------------------------------------------------------------

static JSValue js_mesh_subdivideLoop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = getMW(this_val);
    if (!w) return JS_UNDEFINED;
    int iter = optInt(ctx, argv, argc, 0, 1);
    *w->data = bromesh::subdivideLoop(*w->data, iter);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_mesh_subdivideCatmullClark(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = getMW(this_val);
    if (!w) return JS_UNDEFINED;
    int iter = optInt(ctx, argv, argc, 0, 1);
    *w->data = bromesh::subdivideCatmullClark(*w->data, iter);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_mesh_subdivideMidpoint(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = getMW(this_val);
    if (!w) return JS_UNDEFINED;
    int iter = optInt(ctx, argv, argc, 0, 1);
    *w->data = bromesh::subdivideMidpoint(*w->data, iter);
    return JS_DupValue(ctx, this_val);
}

// ---------------------------------------------------------------------------
// Instance methods — smoothing
// ---------------------------------------------------------------------------

static JSValue js_mesh_smoothLaplacian(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* md = getMD(this_val);
    if (!md) return JS_UNDEFINED;
    float lambda = (float)optNum(ctx, argv, argc, 0, 0.5);
    int iter = optInt(ctx, argv, argc, 1, 1);
    bromesh::smoothLaplacian(*md, lambda, iter);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_mesh_smoothTaubin(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* md = getMD(this_val);
    if (!md) return JS_UNDEFINED;
    float lambda = (float)optNum(ctx, argv, argc, 0, 0.5);
    float mu = (float)optNum(ctx, argv, argc, 1, -0.53);
    int iter = optInt(ctx, argv, argc, 2, 1);
    bromesh::smoothTaubin(*md, lambda, mu, iter);
    return JS_DupValue(ctx, this_val);
}

// ---------------------------------------------------------------------------
// Instance methods — optimization
// ---------------------------------------------------------------------------

static JSValue js_mesh_optimizeVertexCache(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* md = getMD(this_val);
    if (md) bromesh::optimizeVertexCache(*md);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_mesh_optimizeVertexFetch(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* md = getMD(this_val);
    if (md) bromesh::optimizeVertexFetch(*md);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_mesh_optimizeOverdraw(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* md = getMD(this_val);
    if (!md) return JS_UNDEFINED;
    float thresh = (float)optNum(ctx, argv, argc, 0, 1.05);
    bromesh::optimizeOverdraw(*md, thresh);
    return JS_DupValue(ctx, this_val);
}

// ---------------------------------------------------------------------------
// Instance methods — analysis
// ---------------------------------------------------------------------------

static JSValue js_mesh_computeBBox(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* md = getMD(this_val);
    if (!md) return JS_UNDEFINED;
    auto bb = bromesh::computeBBox(*md);
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

static JSValue js_mesh_isManifold(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* md = getMD(this_val);
    return md ? JS_NewBool(ctx, bromesh::isManifold(*md)) : JS_UNDEFINED;
}

static JSValue js_mesh_computeVolume(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* md = getMD(this_val);
    return md ? JS_NewFloat64(ctx, bromesh::computeVolume(*md)) : JS_UNDEFINED;
}

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

// raycast(origin, direction, maxDistance?) — origin/direction are [x,y,z] arrays
static JSValue js_mesh_raycast(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* md = getMD(this_val);
    if (!md || argc < 2) return JS_UNDEFINED;
    float origin[3] = {0,0,0}, dir[3] = {0,0,-1};
    for (int i = 0; i < 3; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[0], i);
        JS_ToFloat64(ctx, (double*)&origin[i], e); // safe: float written via double
        JS_FreeValue(ctx, e);
        e = JS_GetPropertyUint32(ctx, argv[1], i);
        JS_ToFloat64(ctx, (double*)&dir[i], e);
        JS_FreeValue(ctx, e);
    }
    // Fix: use double then cast
    double o[3], d[3];
    for (int i = 0; i < 3; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[0], i);
        JS_ToFloat64(ctx, &o[i], e);
        JS_FreeValue(ctx, e);
        e = JS_GetPropertyUint32(ctx, argv[1], i);
        JS_ToFloat64(ctx, &d[i], e);
        JS_FreeValue(ctx, e);
        origin[i] = (float)o[i];
        dir[i] = (float)d[i];
    }
    float maxDist = (float)optNum(ctx, argv, argc, 2, 0.0);
    auto hit = bromesh::raycast(*md, origin, dir, maxDist);
    return makeRayHit(ctx, hit);
}

static JSValue js_mesh_raycastAll(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* md = getMD(this_val);
    if (!md || argc < 2) return JS_UNDEFINED;
    float origin[3], dir[3];
    double tmp;
    for (int i = 0; i < 3; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[0], i);
        JS_ToFloat64(ctx, &tmp, e); origin[i] = (float)tmp; JS_FreeValue(ctx, e);
        e = JS_GetPropertyUint32(ctx, argv[1], i);
        JS_ToFloat64(ctx, &tmp, e); dir[i] = (float)tmp; JS_FreeValue(ctx, e);
    }
    float maxDist = (float)optNum(ctx, argv, argc, 2, 0.0);
    auto hits = bromesh::raycastAll(*md, origin, dir, maxDist);
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < hits.size(); i++) {
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, makeRayHit(ctx, hits[i]));
    }
    return arr;
}

static JSValue js_mesh_raycastTest(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* md = getMD(this_val);
    if (!md || argc < 2) return JS_UNDEFINED;
    float origin[3], dir[3];
    double tmp;
    for (int i = 0; i < 3; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[0], i);
        JS_ToFloat64(ctx, &tmp, e); origin[i] = (float)tmp; JS_FreeValue(ctx, e);
        e = JS_GetPropertyUint32(ctx, argv[1], i);
        JS_ToFloat64(ctx, &tmp, e); dir[i] = (float)tmp; JS_FreeValue(ctx, e);
    }
    float maxDist = (float)optNum(ctx, argv, argc, 2, 0.0);
    return JS_NewBool(ctx, bromesh::raycastTest(*md, origin, dir, maxDist));
}

static JSValue js_mesh_closestPoint(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* md = getMD(this_val);
    if (!md || argc < 1) return JS_UNDEFINED;
    float pt[3];
    double tmp;
    for (int i = 0; i < 3; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[0], i);
        JS_ToFloat64(ctx, &tmp, e); pt[i] = (float)tmp; JS_FreeValue(ctx, e);
    }
    return makeRayHit(ctx, bromesh::closestPoint(*md, pt));
}

static JSValue js_mesh_hasSelfIntersections(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* md = getMD(this_val);
    return md ? JS_NewBool(ctx, bromesh::hasSelfIntersections(*md)) : JS_UNDEFINED;
}

static JSValue js_mesh_findSelfIntersections(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* md = getMD(this_val);
    if (!md) return JS_UNDEFINED;
    auto pairs = bromesh::findSelfIntersections(*md);
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < pairs.size(); i++) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "triA", JS_NewInt32(ctx, (int32_t)pairs[i].triA));
        JS_SetPropertyStr(ctx, obj, "triB", JS_NewInt32(ctx, (int32_t)pairs[i].triB));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, obj);
    }
    return arr;
}

static JSValue js_mesh_intersectsMesh(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* md = getMD(this_val);
    if (!md || argc < 1) return JS_UNDEFINED;
    auto* other = getMD(argv[0]);
    if (!other) return JS_ThrowTypeError(ctx, "argument must be a Mesh");
    return JS_NewBool(ctx, bromesh::meshesIntersect(*md, *other));
}

// ---------------------------------------------------------------------------
// Instance methods — UV
// ---------------------------------------------------------------------------

static JSValue js_mesh_unwrapUVs(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* md = getMD(this_val);
    if (!md) return JS_UNDEFINED;
    auto result = bromesh::unwrapUVs(*md);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "atlasWidth", JS_NewInt32(ctx, result.atlasWidth));
    JS_SetPropertyStr(ctx, obj, "atlasHeight", JS_NewInt32(ctx, result.atlasHeight));
    JS_SetPropertyStr(ctx, obj, "chartCount", JS_NewInt32(ctx, result.chartCount));
    JS_SetPropertyStr(ctx, obj, "success", JS_NewBool(ctx, result.success));
    return obj;
}

static JSValue js_mesh_projectUVs(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* md = getMD(this_val);
    if (!md || argc < 1) return JS_UNDEFINED;
    std::string type = toStr(ctx, argv[0]);
    bromesh::ProjectionType pt = bromesh::ProjectionType::Box;
    if (type == "box")         pt = bromesh::ProjectionType::Box;
    else if (type == "planarXY") pt = bromesh::ProjectionType::PlanarXY;
    else if (type == "planarXZ") pt = bromesh::ProjectionType::PlanarXZ;
    else if (type == "planarYZ") pt = bromesh::ProjectionType::PlanarYZ;
    else if (type == "cylindrical") pt = bromesh::ProjectionType::Cylindrical;
    else if (type == "spherical")   pt = bromesh::ProjectionType::Spherical;
    float scale = (float)optNum(ctx, argv, argc, 1, 1.0);
    bromesh::projectUVs(*md, pt, scale);
    return JS_DupValue(ctx, this_val);
}

// ---------------------------------------------------------------------------
// Static methods — primitives
// ---------------------------------------------------------------------------

static JSValue js_mesh_box(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    float hw = (float)optNum(ctx, argv, argc, 0, 0.5);
    float hh = (float)optNum(ctx, argv, argc, 1, 0.5);
    float hd = (float)optNum(ctx, argv, argc, 2, 0.5);
    return wrapMesh(ctx, bromesh::box(hw, hh, hd));
}

static JSValue js_mesh_sphere(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    float r = (float)optNum(ctx, argv, argc, 0, 0.5);
    int seg = optInt(ctx, argv, argc, 1, 16);
    int rings = optInt(ctx, argv, argc, 2, 12);
    return wrapMesh(ctx, bromesh::sphere(r, seg, rings));
}

static JSValue js_mesh_cylinder(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    float r = (float)optNum(ctx, argv, argc, 0, 0.5);
    float hh = (float)optNum(ctx, argv, argc, 1, 0.5);
    int seg = optInt(ctx, argv, argc, 2, 16);
    return wrapMesh(ctx, bromesh::cylinder(r, hh, seg));
}

static JSValue js_mesh_capsule(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    float r = (float)optNum(ctx, argv, argc, 0, 0.5);
    float hh = (float)optNum(ctx, argv, argc, 1, 0.5);
    int seg = optInt(ctx, argv, argc, 2, 16);
    int rings = optInt(ctx, argv, argc, 3, 8);
    return wrapMesh(ctx, bromesh::capsule(r, hh, seg, rings));
}

static JSValue js_mesh_plane(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    float hw = (float)optNum(ctx, argv, argc, 0, 5.0);
    float hd = (float)optNum(ctx, argv, argc, 1, 5.0);
    int sx = optInt(ctx, argv, argc, 2, 1);
    int sz = optInt(ctx, argv, argc, 3, 1);
    return wrapMesh(ctx, bromesh::plane(hw, hd, sx, sz));
}

static JSValue js_mesh_torus(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    float major = (float)optNum(ctx, argv, argc, 0, 1.0);
    float minor = (float)optNum(ctx, argv, argc, 1, 0.3);
    int majSeg = optInt(ctx, argv, argc, 2, 24);
    int minSeg = optInt(ctx, argv, argc, 3, 12);
    return wrapMesh(ctx, bromesh::torus(major, minor, majSeg, minSeg));
}

static JSValue js_mesh_heightmapGrid(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "heightmapGrid requires (heights, gridW, gridH)");
    std::vector<float> heights;
    if (!readFloatArrayVal(ctx, argv[0], heights)) return JS_ThrowTypeError(ctx, "heights must be Float32Array");
    int gw = optInt(ctx, argv, argc, 1, 0);
    int gh = optInt(ctx, argv, argc, 2, 0);
    float cs = (float)optNum(ctx, argv, argc, 3, 1.0);
    return wrapMesh(ctx, bromesh::heightmapGrid(heights.data(), gw, gh, cs));
}

// ---------------------------------------------------------------------------
// Static methods — CSG
// ---------------------------------------------------------------------------

static JSValue js_mesh_union(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "union requires two Mesh arguments");
    auto* a = getMD(argv[0]);
    auto* b = getMD(argv[1]);
    if (!a || !b) return JS_ThrowTypeError(ctx, "arguments must be Mesh instances");
    return wrapMesh(ctx, bromesh::booleanUnion(*a, *b));
}

static JSValue js_mesh_subtract(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "subtract requires two Mesh arguments");
    auto* a = getMD(argv[0]);
    auto* b = getMD(argv[1]);
    if (!a || !b) return JS_ThrowTypeError(ctx, "arguments must be Mesh instances");
    return wrapMesh(ctx, bromesh::booleanDifference(*a, *b));
}

static JSValue js_mesh_intersect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "intersect requires two Mesh arguments");
    auto* a = getMD(argv[0]);
    auto* b = getMD(argv[1]);
    if (!a || !b) return JS_ThrowTypeError(ctx, "arguments must be Mesh instances");
    return wrapMesh(ctx, bromesh::booleanIntersection(*a, *b));
}

static JSValue js_mesh_splitByPlane(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "splitByPlane requires a Mesh argument");
    auto* md = getMD(argv[0]);
    if (!md) return JS_ThrowTypeError(ctx, "first argument must be a Mesh");
    float nx = (float)optNum(ctx, argv, argc, 1, 0);
    float ny = (float)optNum(ctx, argv, argc, 2, 1);
    float nz = (float)optNum(ctx, argv, argc, 3, 0);
    float offset = (float)optNum(ctx, argv, argc, 4, 0);
    auto [front, back] = bromesh::splitByPlane(*md, nx, ny, nz, offset);
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, wrapMesh(ctx, std::move(front)));
    JS_SetPropertyUint32(ctx, arr, 1, wrapMesh(ctx, std::move(back)));
    return arr;
}

static JSValue js_mesh_merge(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsArray(argv[0]))
        return JS_ThrowTypeError(ctx, "merge requires an array of Mesh instances");
    JSValue lenVal = JS_GetPropertyStr(ctx, argv[0], "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);

    // Merge by appending all into one MeshData
    bromesh::MeshData result;
    for (int32_t i = 0; i < len; i++) {
        JSValue elem = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        auto* md = getMD(elem);
        if (md) {
            uint32_t baseVertex = (uint32_t)(result.positions.size() / 3);
            result.positions.insert(result.positions.end(), md->positions.begin(), md->positions.end());
            result.normals.insert(result.normals.end(), md->normals.begin(), md->normals.end());
            result.uvs.insert(result.uvs.end(), md->uvs.begin(), md->uvs.end());
            result.colors.insert(result.colors.end(), md->colors.begin(), md->colors.end());
            for (auto idx : md->indices) {
                result.indices.push_back(idx + baseVertex);
            }
        }
        JS_FreeValue(ctx, elem);
    }
    return wrapMesh(ctx, std::move(result));
}

// ---------------------------------------------------------------------------
// Static methods — isosurface
// ---------------------------------------------------------------------------

static JSValue js_mesh_marchingCubes(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "marchingCubes requires (field, gridX, gridY, gridZ)");
    std::vector<float> field;
    if (!readFloatArrayVal(ctx, argv[0], field))
        return JS_ThrowTypeError(ctx, "field must be Float32Array");
    int gx = optInt(ctx, argv, argc, 1, 0);
    int gy = optInt(ctx, argv, argc, 2, 0);
    int gz = optInt(ctx, argv, argc, 3, 0);
    float iso = (float)optNum(ctx, argv, argc, 4, 0.0);
    float cs = (float)optNum(ctx, argv, argc, 5, 1.0);
    return wrapMesh(ctx, bromesh::marchingCubes(field.data(), gx, gy, gz, iso, cs));
}

static JSValue js_mesh_dualContour(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "dualContour requires (field, gridX, gridY, gridZ)");
    std::vector<float> field;
    if (!readFloatArrayVal(ctx, argv[0], field))
        return JS_ThrowTypeError(ctx, "field must be Float32Array");
    int gx = optInt(ctx, argv, argc, 1, 0);
    int gy = optInt(ctx, argv, argc, 2, 0);
    int gz = optInt(ctx, argv, argc, 3, 0);
    float iso = (float)optNum(ctx, argv, argc, 4, 0.0);
    float cs = (float)optNum(ctx, argv, argc, 5, 1.0);
    return wrapMesh(ctx, bromesh::dualContour(field.data(), gx, gy, gz, iso, cs));
}

static JSValue js_mesh_greedyMesh(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "greedyMesh requires (voxels, gridX, gridY, gridZ)");
    std::vector<uint8_t> voxels;
    if (!readUint8ArrayVal(ctx, argv[0], voxels))
        return JS_ThrowTypeError(ctx, "voxels must be Uint8Array");
    int gx = optInt(ctx, argv, argc, 1, 0);
    int gy = optInt(ctx, argv, argc, 2, 0);
    int gz = optInt(ctx, argv, argc, 3, 0);
    float cs = (float)optNum(ctx, argv, argc, 4, 1.0);
    // Optional palette
    std::vector<float> palette;
    int palCount = 0;
    if (argc > 5) {
        readFloatArrayVal(ctx, argv[5], palette);
        palCount = optInt(ctx, argv, argc, 6, (int)(palette.size() / 4));
    }
    return wrapMesh(ctx, bromesh::greedyMesh(
        voxels.data(), gx, gy, gz, cs,
        palette.empty() ? nullptr : palette.data(), palCount));
}

// ---------------------------------------------------------------------------
// Static methods — I/O
// ---------------------------------------------------------------------------

static JSValue js_mesh_loadGLTF(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "loadGLTF requires a path");
    auto scene = bromesh::loadGLTF(toStr(ctx, argv[0]));
    JSValue obj = JS_NewObject(ctx);
    JSValue meshArr = JS_NewArray(ctx);
    for (size_t i = 0; i < scene.meshes.size(); i++) {
        JS_SetPropertyUint32(ctx, meshArr, (uint32_t)i, wrapMesh(ctx, std::move(scene.meshes[i])));
    }
    JS_SetPropertyStr(ctx, obj, "meshes", meshArr);
    return obj;
}

static JSValue js_mesh_loadOBJ(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "loadOBJ requires a path");
    return wrapMesh(ctx, bromesh::loadOBJ(toStr(ctx, argv[0])));
}

static JSValue js_mesh_loadFBX(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "loadFBX requires a path");
    auto meshes = bromesh::loadFBX(toStr(ctx, argv[0]));
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < meshes.size(); i++) {
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, wrapMesh(ctx, std::move(meshes[i])));
    }
    return arr;
}

static JSValue js_mesh_loadPLY(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "loadPLY requires a path");
    return wrapMesh(ctx, bromesh::loadPLY(toStr(ctx, argv[0])));
}

static JSValue js_mesh_loadSTL(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "loadSTL requires a path");
    return wrapMesh(ctx, bromesh::loadSTL(toStr(ctx, argv[0])));
}

static JSValue js_mesh_loadVOX(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "loadVOX requires a path");
    auto vox = bromesh::loadVOX(toStr(ctx, argv[0]));
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "sizeX", JS_NewInt32(ctx, vox.sizeX));
    JS_SetPropertyStr(ctx, obj, "sizeY", JS_NewInt32(ctx, vox.sizeY));
    JS_SetPropertyStr(ctx, obj, "sizeZ", JS_NewInt32(ctx, vox.sizeZ));
    // Voxels as Uint8Array
    {
        JSValue abuf = JS_NewArrayBufferCopy(ctx, vox.voxels.data(), vox.voxels.size());
        JSValue arr = JS_NewTypedArray(ctx, 1, &abuf, JS_TYPED_ARRAY_UINT8);
        JS_FreeValue(ctx, abuf);
        JS_SetPropertyStr(ctx, obj, "voxels", arr);
    }
    // Palette as Float32Array (256*4 = 1024 floats)
    {
        size_t bytes = 256 * 4 * sizeof(float);
        JSValue abuf = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(vox.palette), bytes);
        JSValue arr = JS_NewTypedArray(ctx, 1, &abuf, JS_TYPED_ARRAY_FLOAT32);
        JS_FreeValue(ctx, abuf);
        JS_SetPropertyStr(ctx, obj, "palette", arr);
    }
    return obj;
}

// Instance save methods
static JSValue js_mesh_saveGLTF(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* md = getMD(this_val);
    if (!md || argc < 1) return JS_UNDEFINED;
    return JS_NewBool(ctx, bromesh::saveGLTF(*md, toStr(ctx, argv[0])));
}

static JSValue js_mesh_saveOBJ(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* md = getMD(this_val);
    if (!md || argc < 1) return JS_UNDEFINED;
    return JS_NewBool(ctx, bromesh::saveOBJ(*md, toStr(ctx, argv[0])));
}

static JSValue js_mesh_savePLY(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* md = getMD(this_val);
    if (!md || argc < 1) return JS_UNDEFINED;
    return JS_NewBool(ctx, bromesh::savePLY(*md, toStr(ctx, argv[0])));
}

static JSValue js_mesh_saveSTL(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* md = getMD(this_val);
    if (!md || argc < 1) return JS_UNDEFINED;
    return JS_NewBool(ctx, bromesh::saveSTL(*md, toStr(ctx, argv[0])));
}

// ---------------------------------------------------------------------------
// Static method — reconstruct from point cloud
// ---------------------------------------------------------------------------

static JSValue js_mesh_reconstruct(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "reconstruct requires a Mesh (point cloud)");
    auto* md = getMD(argv[0]);
    if (!md) return JS_ThrowTypeError(ctx, "argument must be a Mesh");
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
    return wrapMesh(ctx, bromesh::reconstructFromPointCloud(*md, params));
}

// ---------------------------------------------------------------------------
// Prototype function list
// ---------------------------------------------------------------------------

static const JSCFunctionListEntry js_mesh_proto[] = {
    // Properties
    JS_CGETSET_DEF("positions", js_mesh_get_positions, js_mesh_set_positions),
    JS_CGETSET_DEF("normals", js_mesh_get_normals, js_mesh_set_normals),
    JS_CGETSET_DEF("uvs", js_mesh_get_uvs, js_mesh_set_uvs),
    JS_CGETSET_DEF("colors", js_mesh_get_colors, js_mesh_set_colors),
    JS_CGETSET_DEF("indices", js_mesh_get_indices, js_mesh_set_indices),
    JS_CGETSET_DEF("vertexCount", js_mesh_get_vertexCount, nullptr),
    JS_CGETSET_DEF("triangleCount", js_mesh_get_triangleCount, nullptr),
    JS_CGETSET_DEF("hasNormals", js_mesh_get_hasNormals, nullptr),
    JS_CGETSET_DEF("hasUVs", js_mesh_get_hasUVs, nullptr),
    JS_CGETSET_DEF("hasColors", js_mesh_get_hasColors, nullptr),
    JS_CGETSET_DEF("empty", js_mesh_get_empty, nullptr),

    // Clone
    JS_CFUNC_DEF("clone", 0, js_mesh_clone),

    // Transform
    JS_CFUNC_DEF("translate", 3, js_mesh_translate),
    JS_CFUNC_DEF("scale", 1, js_mesh_scale),
    JS_CFUNC_DEF("rotate", 4, js_mesh_rotate),
    JS_CFUNC_DEF("center", 0, js_mesh_center),
    JS_CFUNC_DEF("mirror", 1, js_mesh_mirror),
    JS_CFUNC_DEF("transform", 1, js_mesh_transform),

    // Normals
    JS_CFUNC_DEF("computeNormals", 0, js_mesh_computeNormals),
    JS_CFUNC_DEF("computeFlatNormals", 0, js_mesh_computeFlatNormals),
    JS_CFUNC_DEF("computeTangents", 0, js_mesh_computeTangents),

    // Simplification
    JS_CFUNC_DEF("simplify", 1, js_mesh_simplify),
    JS_CFUNC_DEF("simplifyWithAttributes", 1, js_mesh_simplifyWithAttributes),
    JS_CFUNC_DEF("simplifyToTriangleCount", 1, js_mesh_simplifyToTriangleCount),
    JS_CFUNC_DEF("generateLODChain", 1, js_mesh_generateLODChain),

    // Subdivision
    JS_CFUNC_DEF("subdivideLoop", 0, js_mesh_subdivideLoop),
    JS_CFUNC_DEF("subdivideCatmullClark", 0, js_mesh_subdivideCatmullClark),
    JS_CFUNC_DEF("subdivideMidpoint", 0, js_mesh_subdivideMidpoint),

    // Smoothing
    JS_CFUNC_DEF("smoothLaplacian", 0, js_mesh_smoothLaplacian),
    JS_CFUNC_DEF("smoothTaubin", 0, js_mesh_smoothTaubin),

    // Optimization
    JS_CFUNC_DEF("optimizeVertexCache", 0, js_mesh_optimizeVertexCache),
    JS_CFUNC_DEF("optimizeVertexFetch", 0, js_mesh_optimizeVertexFetch),
    JS_CFUNC_DEF("optimizeOverdraw", 0, js_mesh_optimizeOverdraw),

    // Analysis
    JS_CFUNC_DEF("computeBBox", 0, js_mesh_computeBBox),
    JS_CFUNC_DEF("isManifold", 0, js_mesh_isManifold),
    JS_CFUNC_DEF("computeVolume", 0, js_mesh_computeVolume),
    JS_CFUNC_DEF("raycast", 2, js_mesh_raycast),
    JS_CFUNC_DEF("raycastAll", 2, js_mesh_raycastAll),
    JS_CFUNC_DEF("raycastTest", 2, js_mesh_raycastTest),
    JS_CFUNC_DEF("closestPoint", 1, js_mesh_closestPoint),
    JS_CFUNC_DEF("hasSelfIntersections", 0, js_mesh_hasSelfIntersections),
    JS_CFUNC_DEF("findSelfIntersections", 0, js_mesh_findSelfIntersections),
    JS_CFUNC_DEF("intersectsMesh", 1, js_mesh_intersectsMesh),

    // UV
    JS_CFUNC_DEF("unwrapUVs", 0, js_mesh_unwrapUVs),
    JS_CFUNC_DEF("projectUVs", 1, js_mesh_projectUVs),

    // I/O (save)
    JS_CFUNC_DEF("saveGLTF", 1, js_mesh_saveGLTF),
    JS_CFUNC_DEF("saveOBJ", 1, js_mesh_saveOBJ),
    JS_CFUNC_DEF("savePLY", 1, js_mesh_savePLY),
    JS_CFUNC_DEF("saveSTL", 1, js_mesh_saveSTL),
};

// ---------------------------------------------------------------------------
// Wrap helper (creates a JS Mesh from MeshData)
// ---------------------------------------------------------------------------

static JSValue s_mesh_proto = JS_UNDEFINED;

static JSValue wrapMesh(JSContext* ctx, bromesh::MeshData&& data) {
    JSValue obj = JS_NewObjectClass(ctx, js_mesh_class_id);
    JS_SetPrototype(ctx, obj, JS_DupValue(ctx, s_mesh_proto));
    auto* w = new MeshWrapper{std::make_unique<bromesh::MeshData>(std::move(data))};
    JS_SetOpaque(obj, w);
    return obj;
}

// ---------------------------------------------------------------------------
// Install / Cleanup
// ---------------------------------------------------------------------------

static JSValue s_mesh_ctor = JS_UNDEFINED;

void MeshBindings::install(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    JS_NewClassID(rt, &js_mesh_class_id);
    JS_NewClass(rt, js_mesh_class_id, &js_mesh_class);

    // Prototype
    s_mesh_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, s_mesh_proto,
                               js_mesh_proto, sizeof(js_mesh_proto) / sizeof(js_mesh_proto[0]));

    // Constructor
    s_mesh_ctor = JS_NewCFunction2(ctx, js_mesh_ctor, "Mesh", 1,
                                   JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, s_mesh_ctor, s_mesh_proto);

    // Static methods on the constructor
    JS_SetPropertyStr(ctx, s_mesh_ctor, "box",
        JS_NewCFunction(ctx, js_mesh_box, "box", 0));
    JS_SetPropertyStr(ctx, s_mesh_ctor, "sphere",
        JS_NewCFunction(ctx, js_mesh_sphere, "sphere", 0));
    JS_SetPropertyStr(ctx, s_mesh_ctor, "cylinder",
        JS_NewCFunction(ctx, js_mesh_cylinder, "cylinder", 0));
    JS_SetPropertyStr(ctx, s_mesh_ctor, "capsule",
        JS_NewCFunction(ctx, js_mesh_capsule, "capsule", 0));
    JS_SetPropertyStr(ctx, s_mesh_ctor, "plane",
        JS_NewCFunction(ctx, js_mesh_plane, "plane", 0));
    JS_SetPropertyStr(ctx, s_mesh_ctor, "torus",
        JS_NewCFunction(ctx, js_mesh_torus, "torus", 0));
    JS_SetPropertyStr(ctx, s_mesh_ctor, "heightmapGrid",
        JS_NewCFunction(ctx, js_mesh_heightmapGrid, "heightmapGrid", 3));

    // Static — CSG
    JS_SetPropertyStr(ctx, s_mesh_ctor, "union",
        JS_NewCFunction(ctx, js_mesh_union, "union", 2));
    JS_SetPropertyStr(ctx, s_mesh_ctor, "subtract",
        JS_NewCFunction(ctx, js_mesh_subtract, "subtract", 2));
    JS_SetPropertyStr(ctx, s_mesh_ctor, "intersect",
        JS_NewCFunction(ctx, js_mesh_intersect, "intersect", 2));
    JS_SetPropertyStr(ctx, s_mesh_ctor, "splitByPlane",
        JS_NewCFunction(ctx, js_mesh_splitByPlane, "splitByPlane", 5));
    JS_SetPropertyStr(ctx, s_mesh_ctor, "merge",
        JS_NewCFunction(ctx, js_mesh_merge, "merge", 1));

    // Static — isosurface
    JS_SetPropertyStr(ctx, s_mesh_ctor, "marchingCubes",
        JS_NewCFunction(ctx, js_mesh_marchingCubes, "marchingCubes", 4));
    JS_SetPropertyStr(ctx, s_mesh_ctor, "dualContour",
        JS_NewCFunction(ctx, js_mesh_dualContour, "dualContour", 4));
    JS_SetPropertyStr(ctx, s_mesh_ctor, "greedyMesh",
        JS_NewCFunction(ctx, js_mesh_greedyMesh, "greedyMesh", 4));

    // Static — I/O (load)
    JS_SetPropertyStr(ctx, s_mesh_ctor, "loadGLTF",
        JS_NewCFunction(ctx, js_mesh_loadGLTF, "loadGLTF", 1));
    JS_SetPropertyStr(ctx, s_mesh_ctor, "loadOBJ",
        JS_NewCFunction(ctx, js_mesh_loadOBJ, "loadOBJ", 1));
    JS_SetPropertyStr(ctx, s_mesh_ctor, "loadFBX",
        JS_NewCFunction(ctx, js_mesh_loadFBX, "loadFBX", 1));
    JS_SetPropertyStr(ctx, s_mesh_ctor, "loadPLY",
        JS_NewCFunction(ctx, js_mesh_loadPLY, "loadPLY", 1));
    JS_SetPropertyStr(ctx, s_mesh_ctor, "loadSTL",
        JS_NewCFunction(ctx, js_mesh_loadSTL, "loadSTL", 1));
    JS_SetPropertyStr(ctx, s_mesh_ctor, "loadVOX",
        JS_NewCFunction(ctx, js_mesh_loadVOX, "loadVOX", 1));

    // Static — reconstruction
    JS_SetPropertyStr(ctx, s_mesh_ctor, "reconstruct",
        JS_NewCFunction(ctx, js_mesh_reconstruct, "reconstruct", 1));

    // Set global
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "Mesh", JS_DupValue(ctx, s_mesh_ctor));
    JS_FreeValue(ctx, global);
}

void MeshBindings::cleanup(JSContext* ctx) {
    JS_FreeValue(ctx, s_mesh_proto);
    JS_FreeValue(ctx, s_mesh_ctor);
    s_mesh_proto = JS_UNDEFINED;
    s_mesh_ctor = JS_UNDEFINED;
}

bromesh::MeshData* MeshBindings::getMeshData(JSContext* ctx, JSValueConst val) {
    (void)ctx;
    return getMD(val);
}

JSClassID MeshBindings::classId() {
    return js_mesh_class_id;
}

} // namespace bro::js
