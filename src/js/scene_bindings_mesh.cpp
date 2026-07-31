// Scene JS bindings — MeshNode / SkinnedMeshNode / InstancedMeshNode surface:
// creation, the shared createMesh option parser (material, textures, mesh
// data), runtime mesh/texture updates, GPU skinning palette upload, and the
// per-instance buffer setters. Shared wrapper structs + helpers live in
// scene_bindings_internal.h.

#include "js/scene_bindings.h"
#if BRO_WITH_3D  // modular-build feature gate
#include "js/scene_bindings_internal.h"
#include "js/mesh_bindings.h"
#include "js/rigging_bindings.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/mesh_node.h"
#include "scene/skinned_mesh_node.h"
#include "scene/instanced_mesh_node.h"
#include "scene/decal_node.h"

#include <algorithm>
#include <qjsbind/qjsbind.h>

#include <bromesh/primitives/primitives.h>
#include <bromesh/manipulation/normals.h>

#include <climits>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bro::js {

// --- Forward decls used by updateMesh (defined later) ---
static bool jsReadFloatArray(JSContext* ctx, JSValueConst obj, const char* prop,
                             std::vector<float>& out);
static bool jsReadUint32Array(JSContext* ctx, JSValueConst obj, const char* prop,
                              std::vector<uint32_t>& out);

// setBaseColorTexture(tex|sceneTexture|null) — replace or clear the baseColor
// texture at runtime. `tex` shape matches createMesh's `texture` option:
// { width, height, data: Uint8Array(rgba8) }. A SceneTexture handle from
// sceneGraph.asTexture() installs a live link to that scene's LDR output
// instead (re-resolved every frame — survives source resize / renderScale
// changes, falls back to base color when the source scene is destroyed).
// Pass null/undefined to clear so the mesh falls back to `uColor` (and
// vertex colors if present).
JSValue js_node_setBaseColorTexture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node() ||
        (w->node()->type() != scene::SceneNode::Type::Mesh &&
         w->node()->type() != scene::SceneNode::Type::Decal))
        return JS_ThrowTypeError(ctx, "setBaseColorTexture: not a MeshNode or DecalNode");

    // DecalNode: runtime albedo swap — bytes shape only (no scene-as-texture
    // link for decals). Null/undefined clears back to plain modulate.
    if (w->node()->type() == scene::SceneNode::Type::Decal) {
        auto* decal = static_cast<scene::DecalNode*>(w->node());
        if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
            decal->clearAlbedoTexture();
            return JS_UNDEFINED;
        }
        if (!JS_IsObject(argv[0]))
            return JS_ThrowTypeError(ctx, "setBaseColorTexture: expected { width, height, data } or null");
        int w_ = (int)qjsbind::get_prop_number(ctx, argv[0], "width",  0);
        int h_ = (int)qjsbind::get_prop_number(ctx, argv[0], "height", 0);
        JSValue dataVal = JS_GetPropertyStr(ctx, argv[0], "data");
        size_t off = 0, len = 0;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, dataVal, &off, &len, nullptr);
        if (!JS_IsException(ab)) {
            size_t bytes = 0;
            uint8_t* base = JS_GetArrayBuffer(ctx, &bytes, ab);
            if (base && w_ > 0 && h_ > 0 && len >= (size_t)w_ * (size_t)h_ * 4) {
                decal->setAlbedoTexture(w_, h_, base + off);
            }
            JS_FreeValue(ctx, ab);
        }
        JS_FreeValue(ctx, dataVal);
        return JS_UNDEFINED;
    }

    auto* meshNode = static_cast<scene::MeshNode*>(w->node());

    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        meshNode->clearBaseColorTexture();
        return JS_UNDEFINED;
    }
    // Scene-as-texture: a SceneTexture handle from sceneGraph.asTexture().
    // The provider captures the weak liveness token by value, so the mesh's
    // link is independent of the JS handle object's lifetime (it may be GC'd
    // immediately) and never keeps the source SceneGraph alive.
    if (auto* h = qjsbind::unwrap<SceneTextureHandle>(ctx, argv[0])) {
        std::weak_ptr<scene::SceneGraph::OutputTextureSource> wk = h->src;
        meshNode->setExternalBaseColorTexture([wk]() -> unsigned {
            auto s = wk.lock();
            return (s && s->graph) ? s->graph->outputColorTexture() : 0u;
        });
        return JS_UNDEFINED;
    }
    if (!JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "setBaseColorTexture: expected { width, height, data }, SceneTexture, or null");

    int w_ = (int)qjsbind::get_prop_number(ctx, argv[0], "width",  0);
    int h_ = (int)qjsbind::get_prop_number(ctx, argv[0], "height", 0);
    JSValue dataVal = JS_GetPropertyStr(ctx, argv[0], "data");
    size_t off = 0, len = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, dataVal, &off, &len, nullptr);
    if (!JS_IsException(ab)) {
        size_t bytes = 0;
        uint8_t* base = JS_GetArrayBuffer(ctx, &bytes, ab);
        if (base && w_ > 0 && h_ > 0 && len >= (size_t)w_ * (size_t)h_ * 4) {
            meshNode->setBaseColorTexture(w_, h_, base + off);
        }
        JS_FreeValue(ctx, ab);
    }
    JS_FreeValue(ctx, dataVal);
    return JS_UNDEFINED;
}

// --- Custom shaders (static MeshNode only) ----------------------------------

// Parse one user-uniform value: number → float (1 comp), Array of 1..4
// numbers → vecN. Returns the component count, 0 on an invalid shape.
static int parseShaderUniformValue(JSContext* ctx, JSValueConst val, float out[4]) {
    if (JS_IsNumber(val)) {
        out[0] = (float)jsNum(ctx, val);
        return 1;
    }
    if (JS_IsArray(val)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, val, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        if (len < 1 || len > 4) return 0;
        for (int32_t i = 0; i < len; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, val, (uint32_t)i);
            out[i] = (float)jsNum(ctx, e);
            JS_FreeValue(ctx, e);
        }
        return (int)len;
    }
    return 0;
}

// User uniforms live in a reserved `u_` namespace so they can never collide
// with the engine's own uniforms (camelCase `uMVP`-style, no underscore).
static bool validUserUniformName(const std::string& n) {
    return n.size() >= 3 && n[0] == 'u' && n[1] == '_';
}

// True when the node carries the custom-shader surface (MeshNode incl.
// skinned, or InstancedMeshNode).
static bool isShaderableNode(const NodeWrapper* w) {
    return w && w->node() &&
           (w->node()->type() == scene::SceneNode::Type::Mesh ||
            w->node()->type() == scene::SceneNode::Type::InstancedMesh);
}

// setShader({ vertex?, fragment?, uniforms? }) — install user GLSL chunks on
// a MeshNode (static or skinned) or an InstancedMeshNode. Compiles every
// program variant the node can render with eagerly (the JS thread owns the
// GL context); on GLSL compile/link failure throws a SyntaxError carrying
// the full driver log and leaves the node's previous shader (or the default
// pipeline) untouched. See docs/scene-api.js for the hook-point contract.
JSValue js_node_setShader(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!isShaderableNode(w))
        return JS_ThrowTypeError(ctx,
            "setShader: not a MeshNode or InstancedMeshNode");
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "setShader: expected { vertex?, fragment?, uniforms? }");

    std::string vs, fs;
    const std::pair<const char*, std::string*> chunkProps[] = {
        {"vertex", &vs}, {"fragment", &fs}};
    for (const auto& [prop, dst] : chunkProps) {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], prop);
        bool bad = !JS_IsUndefined(v) && !JS_IsNull(v) && !JS_IsString(v);
        if (JS_IsString(v)) *dst = jsStr(ctx, v);
        JS_FreeValue(ctx, v);
        if (bad)
            return JS_ThrowTypeError(ctx, "setShader: %s must be a GLSL string", prop);
    }
    if (vs.empty() && fs.empty())
        return JS_ThrowTypeError(ctx,
            "setShader: at least one of vertex/fragment is required");

    // Parse the uniforms object up-front so a malformed entry throws before
    // anything is applied (the call is atomic: all or nothing).
    std::vector<scene::CustomShaderUniform> uniforms;
    std::string badUniform;
    JSValue uobj = JS_GetPropertyStr(ctx, argv[0], "uniforms");
    if (JS_IsObject(uobj)) {
        JSPropertyEnum* props = nullptr;
        uint32_t plen = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &plen, uobj,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < plen; i++) {
                if (badUniform.empty()) {
                    JSValue key = JS_AtomToValue(ctx, props[i].atom);
                    std::string name = jsStr(ctx, key);
                    JS_FreeValue(ctx, key);
                    scene::CustomShaderUniform u;
                    u.name = name;
                    JSValue uval = JS_GetProperty(ctx, uobj, props[i].atom);
                    u.comps = parseShaderUniformValue(ctx, uval, u.v);
                    JS_FreeValue(ctx, uval);
                    if (u.comps > 0 && validUserUniformName(name)) {
                        uniforms.push_back(std::move(u));
                    } else {
                        badUniform = name;
                    }
                }
                JS_FreeAtom(ctx, props[i].atom);
            }
            js_free(ctx, props);
        }
    }
    JS_FreeValue(ctx, uobj);
    if (!badUniform.empty())
        return JS_ThrowTypeError(ctx,
            "setShader: uniform '%s' must use the u_ prefix and be a number "
            "or an array of 1-4 numbers", badUniform.c_str());

    // Eager compile of every variant this node can render with — the
    // programs land in the renderer's cache keyed by the chunk sources, so
    // the draw path finds them without recompiling. Skinned nodes compile
    // Static AND Skinned (a not-ready skin degrades to the static path).
    // Any failure throws before anything is applied.
    std::string key = vs + '\x1f' + fs;
    std::string err;
    if (!w->graph())
        return JS_ThrowSyntaxError(ctx, "setShader: no scene graph");
    using Target = scene::SceneRenderer::CustomShaderTarget;
    if (w->node()->type() == scene::SceneNode::Type::InstancedMesh) {
        auto* instNode = static_cast<scene::InstancedMeshNode*>(w->node());
        if (!w->graph()->compileCustomShader(Target::Instanced, key, vs, fs, err))
            return JS_ThrowSyntaxError(ctx, "%s", err.c_str());
        instNode->setCustomShader(std::move(vs), std::move(fs));
        for (const auto& u : uniforms)
            instNode->setCustomShaderUniform(u.name, u.comps, u.v);
        return JS_DupValue(ctx, this_val);
    }
    auto* meshNode = static_cast<scene::MeshNode*>(w->node());
    if (!w->graph()->compileCustomShader(Target::Static, key, vs, fs, err))
        return JS_ThrowSyntaxError(ctx, "%s", err.c_str());
    if (meshNode->asSkinnedMesh() &&
        !w->graph()->compileCustomShader(Target::Skinned, key, vs, fs, err))
        return JS_ThrowSyntaxError(ctx, "%s", err.c_str());

    meshNode->setCustomShader(std::move(vs), std::move(fs));
    for (const auto& u : uniforms)
        meshNode->setCustomShaderUniform(u.name, u.comps, u.v);
    return JS_DupValue(ctx, this_val);
}

// setShaderUniform(name, value) — update one numeric user uniform. value is
// a number (float) or an array of 1-4 numbers (vec2/3/4). Values live on the
// node, so meshes sharing a program keep independent values.
JSValue js_node_setShaderUniform(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!isShaderableNode(w))
        return JS_ThrowTypeError(ctx,
            "setShaderUniform: not a MeshNode or InstancedMeshNode");
    const bool inst = w->node()->type() == scene::SceneNode::Type::InstancedMesh;
    const bool hasShader =
        inst ? static_cast<scene::InstancedMeshNode*>(w->node())->hasCustomShader()
             : static_cast<scene::MeshNode*>(w->node())->hasCustomShader();
    if (!hasShader)
        return JS_ThrowTypeError(ctx,
            "setShaderUniform: no custom shader set (call setShader first)");
    if (argc < 2 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "setShaderUniform: expected (name, value)");
    std::string name = jsStr(ctx, argv[0]);
    if (!validUserUniformName(name))
        return JS_ThrowTypeError(ctx,
            "setShaderUniform: uniform name must use the u_ prefix (got '%s')",
            name.c_str());
    float v[4] = {};
    int comps = parseShaderUniformValue(ctx, argv[1], v);
    if (comps == 0)
        return JS_ThrowTypeError(ctx,
            "setShaderUniform: value must be a number or an array of 1-4 numbers");
    if (inst)
        static_cast<scene::InstancedMeshNode*>(w->node())
            ->setCustomShaderUniform(name, comps, v);
    else
        static_cast<scene::MeshNode*>(w->node())
            ->setCustomShaderUniform(name, comps, v);
    return JS_DupValue(ctx, this_val);
}

// setShaderTexture(name, { width, height, data: Float32Array,
//                          mipmap?, x?, y? } | null) —
// bind a single-channel float texture to a `uniform sampler2D u_*` declared
// by the custom shader. Uploaded as R32F / GL_RED / GL_FLOAT with LINEAR
// filtering and CLAMP_TO_EDGE, so a heightfield raymarcher gets bilinear
// samples and no wrap-around at the borders. Pass null to release.
//
// `mipmap: true` generates a mip chain and switches minification to
// trilinear — required for a shader that calls textureLod() at a fractional
// level, which needs two populated levels to blend between.
//
// `x`/`y` turn the call into a sub-rectangle update of the EXISTING texture
// (glTexSubImage2D, no reallocation, mips regenerated when the slot is
// mipmapped). Presence of either key is the discriminator: a full upload has
// no position, and a sub-update is meaningless without one. An out-of-bounds
// rect, or one against a slot that was never sized, is rejected rather than
// clamped — a silently clipped write would corrupt the region the caller
// believes it just wrote.
//
// User samplers occupy texture units starting ABOVE every unit the material
// uber-shader uses (baseColor 0, shadow atlas 1, IBL 2/3/4, normal 5, MR 6,
// AO 7, emissive 8, reflection probe 9) — see MeshNode::kUserTextureUnitBase.
// Starting at or below those units would not error anywhere: the user texture
// would simply overwrite a material binding and the mesh would silently
// sample a heightfield as its albedo/shadow map. The budget is bounded by the
// QUERIED combined texture image unit count (GL 3.3's 16 is the floor, not the
// assumption — see MeshNode::maxUserTextures), so a node gets whatever this GL
// actually has above the base and asking for more throws rather than clobbering.
JSValue js_node_setShaderTexture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node() || w->node()->type() != scene::SceneNode::Type::Mesh)
        return JS_ThrowTypeError(ctx, "setShaderTexture: not a MeshNode");
    auto* meshNode = static_cast<scene::MeshNode*>(w->node());
    if (!meshNode->hasCustomShader())
        return JS_ThrowTypeError(ctx,
            "setShaderTexture: no custom shader set (call setShader first)");
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx,
            "setShaderTexture: expected (name, { width, height, data } | null)");
    std::string name = jsStr(ctx, argv[0]);
    if (!validUserUniformName(name))
        return JS_ThrowTypeError(ctx,
            "setShaderTexture: uniform name must use the u_ prefix (got '%s')",
            name.c_str());

    if (argc < 2 || JS_IsNull(argv[1]) || JS_IsUndefined(argv[1])) {
        meshNode->clearCustomShaderTexture(name);
        return JS_DupValue(ctx, this_val);
    }
    if (!JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx,
            "setShaderTexture: expected { width, height, data: Float32Array } or null");

    int tw = (int)qjsbind::get_prop_number(ctx, argv[1], "width",  0);
    int th = (int)qjsbind::get_prop_number(ctx, argv[1], "height", 0);
    if (tw <= 0 || th <= 0)
        return JS_ThrowTypeError(ctx,
            "setShaderTexture: width and height must be positive");

    // INT_MIN can't be produced by a caller who meant a coordinate, so it
    // distinguishes "no x/y given" (full upload) from an explicit x: 0.
    constexpr int kNoPos = INT_MIN;
    const int sx = qjsbind::get_prop_int(ctx, argv[1], "x", kNoPos);
    const int sy = qjsbind::get_prop_int(ctx, argv[1], "y", kNoPos);
    const bool isSub = (sx != kNoPos || sy != kNoPos);

    JSValue dataVal = JS_GetPropertyStr(ctx, argv[1], "data");
    size_t off = 0, len = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, dataVal, &off, &len, nullptr);
    JS_FreeValue(ctx, dataVal);
    if (JS_IsException(ab))
        return JS_ThrowTypeError(ctx,
            "setShaderTexture: data must be a Float32Array");
    size_t bytes = 0;
    uint8_t* base = JS_GetArrayBuffer(ctx, &bytes, ab);
    JS_FreeValue(ctx, ab);
    const size_t need = (size_t)tw * (size_t)th * sizeof(float);
    if (!base || len < need)
        return JS_ThrowTypeError(ctx,
            "setShaderTexture: data must hold width*height floats "
            "(%d*%d, got %d)", tw, th, (int)(len / sizeof(float)));

    const float* pixels = reinterpret_cast<const float*>(base + off);

    if (isSub) {
        // Rejection is reported by return value (logged native-side), not by
        // throwing: a streaming caller updating a tile per frame shouldn't
        // have to guard every call once the slot exists.
        meshNode->updateCustomShaderTexture(name,
                                            sx == kNoPos ? 0 : sx,
                                            sy == kNoPos ? 0 : sy,
                                            tw, th, pixels);
        return JS_DupValue(ctx, this_val);
    }

    const bool mipmap = qjsbind::get_prop_bool(ctx, argv[1], "mipmap", false);
    // Wrap mode: a window onto the world clamps (default); a periodic tile
    // repeats; an equirect chart repeats in S but clamps in T (poles are
    // single-valued and must not wrap the wrong hemisphere in).
    const bool repeat = qjsbind::get_prop_bool(ctx, argv[1], "repeat", false);
    const bool clampT = qjsbind::get_prop_bool(ctx, argv[1], "clampT", false);
    if (!meshNode->setCustomShaderTexture(name, tw, th, pixels, mipmap, repeat, clampT))
        return JS_ThrowTypeError(ctx,
            "setShaderTexture: too many sampler uniforms on this node "
            "(max %d; this GL reports %d combined texture units and the "
            "material shader uses the first %d)",
            scene::MeshNode::maxUserTextures(),
            scene::MeshNode::userTextureUnitLimit(),
            scene::MeshNode::kUserTextureUnitBase);
    return JS_DupValue(ctx, this_val);
}

// clearShader() — drop the custom shader (and its uniform values); the mesh
// returns to the default pipeline (including its unlit behavior, if set).
// The cached program stays in the renderer for future reuse.
JSValue js_node_clearShader(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!isShaderableNode(w))
        return JS_ThrowTypeError(ctx,
            "clearShader: not a MeshNode or InstancedMeshNode");
    if (w->node()->type() == scene::SceneNode::Type::InstancedMesh)
        static_cast<scene::InstancedMeshNode*>(w->node())->clearCustomShader();
    else
        static_cast<scene::MeshNode*>(w->node())->clearCustomShader();
    return JS_DupValue(ctx, this_val);
}

// updateMesh(meshOrOpts[, opts])
JSValue js_node_updateMesh(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node())
        return JS_ThrowTypeError(ctx, "updateMesh: invalid node");
    if (w->node()->type() != scene::SceneNode::Type::Mesh)
        return JS_ThrowTypeError(ctx, "updateMesh: node is not a MeshNode");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "updateMesh: missing argument");

    auto* meshNode = static_cast<scene::MeshNode*>(w->node());
    bromesh::MeshData meshData;
    bool gotData = false;

    bool transfer = false;
    bool recomputeNormals = false;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        transfer = qjsbind::get_prop_bool(ctx, argv[1], "transfer", false);
        recomputeNormals = qjsbind::get_prop_bool(ctx, argv[1], "recomputeNormals", false);
    }

    // Path 1: argument is a Mesh object directly.
    if (MeshBindings::getMeshData(ctx, argv[0])) {
        if (transfer) {
            if (auto taken = MeshBindings::takeMeshData(ctx, argv[0])) {
                meshData = std::move(*taken);
                gotData = true;
            }
        } else {
            meshData = *MeshBindings::getMeshData(ctx, argv[0]);
            gotData = true;
        }
    }

    // Path 2: options object with `mesh:`/`data:` (Mesh) or raw typed arrays.
    if (!gotData && JS_IsObject(argv[0])) {
        bool transferOpt = qjsbind::get_prop_bool(ctx, argv[0], "transfer", false);
        recomputeNormals |= qjsbind::get_prop_bool(ctx, argv[0], "recomputeNormals", false);

        auto tryKey = [&](const char* key) -> bool {
            JSValue v = JS_GetPropertyStr(ctx, argv[0], key);
            bool took = false;
            if (!JS_IsUndefined(v) && MeshBindings::getMeshData(ctx, v)) {
                if (transferOpt) {
                    if (auto taken = MeshBindings::takeMeshData(ctx, v)) {
                        meshData = std::move(*taken);
                        took = true;
                    }
                } else {
                    meshData = *MeshBindings::getMeshData(ctx, v);
                    took = true;
                }
            }
            JS_FreeValue(ctx, v);
            return took;
        };
        if (tryKey("mesh") || tryKey("data"))
            gotData = true;

        if (!gotData) {
            std::vector<float> positions, normals;
            std::vector<uint32_t> indices;
            if (jsReadFloatArray(ctx, argv[0], "positions", positions) &&
                jsReadUint32Array(ctx, argv[0], "indices", indices)) {
                meshData.positions = std::move(positions);
                meshData.indices = std::move(indices);
                if (jsReadFloatArray(ctx, argv[0], "normals", normals)) {
                    meshData.normals = std::move(normals);
                }
                gotData = true;
            }
        }
    }

    if (!gotData)
        return JS_ThrowTypeError(ctx, "updateMesh: argument must be a Mesh or {positions,indices}");

    // Deforming geometry (e.g. a soft body streaming vertices() in per frame)
    // needs fresh smooth normals or the lit mesh goes black/faceted.
    if (recomputeNormals && !meshData.positions.empty() && !meshData.indices.empty())
        bromesh::computeNormals(meshData);

    meshNode->setMesh(std::move(meshData));
    return JS_DupValue(ctx, this_val);
}

// setLodMeshes([{mesh, maxDist}, ...]) — install a discrete LOD chain on a
// plain MeshNode (skinned/instanced are rejected — see scene-api.js). Each
// entry's `mesh` must be a Mesh object; `maxDist` is the camera distance the
// level draws up to. An empty array clears the chain (back to the base mesh).
JSValue js_node_setLodMeshes(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node() || w->node()->type() != scene::SceneNode::Type::Mesh)
        return JS_ThrowTypeError(ctx, "setLodMeshes: not a MeshNode");
    auto* meshNode = static_cast<scene::MeshNode*>(w->node());
    if (meshNode->asSkinnedMesh())
        return JS_ThrowTypeError(ctx, "setLodMeshes: not supported on skinned meshes");
    if (argc < 1 || !JS_IsArray(argv[0]))
        return JS_ThrowTypeError(ctx, "setLodMeshes: argument must be an array of {mesh, maxDist}");

    JSValue lenVal = JS_GetPropertyStr(ctx, argv[0], "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);

    std::vector<scene::MeshNode::LodLevel> levels;
    levels.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        JSValue entry = JS_GetPropertyUint32(ctx, argv[0], i);
        if (!JS_IsObject(entry)) {
            JS_FreeValue(ctx, entry);
            return JS_ThrowTypeError(ctx, "setLodMeshes: entry %u is not an object", i);
        }
        JSValue meshVal = JS_GetPropertyStr(ctx, entry, "mesh");
        bromesh::MeshData* md = MeshBindings::getMeshData(ctx, meshVal);
        if (!md) {
            JS_FreeValue(ctx, meshVal);
            JS_FreeValue(ctx, entry);
            return JS_ThrowTypeError(ctx, "setLodMeshes: entry %u has no Mesh in `mesh`", i);
        }
        scene::MeshNode::LodLevel lv;
        lv.mesh = *md;   // copy — the JS Mesh stays usable
        lv.maxDist = (float)qjsbind::get_prop_number(ctx, entry, "maxDist", 1e30);
        JS_FreeValue(ctx, meshVal);
        JS_FreeValue(ctx, entry);
        levels.push_back(std::move(lv));
    }

    meshNode->setLodMeshes(std::move(levels));
    return JS_DupValue(ctx, this_val);
}

// --- Helper: read a typed array property into a vector<float> or vector<uint32_t> ---
static bool jsReadFloatArray(JSContext* ctx, JSValueConst obj, const char* prop,
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

static bool jsReadUint32Array(JSContext* ctx, JSValueConst obj, const char* prop,
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

// Apply the full createMesh option surface — name, transform, color, PBR
// material, draw flags, mesh data, texture maps — to a MeshNode. Split out
// so createSkinnedMesh carries the identical material API without duplicating
// the parsing.
static void applyMeshNodeOptions(JSContext* ctx, scene::MeshNode* node,
                                 JSValueConst opts) {
    {
        JSValue nameVal = JS_GetPropertyStr(ctx, opts, "name");
        if (JS_IsString(nameVal)) node->setName(jsStr(ctx, nameVal));
        JS_FreeValue(ctx, nameVal);

        // Visibility. Only honoured when actually present, so a node that
        // omits the option keeps the constructor's default rather than being
        // forced visible by a JS_ToBool on undefined.
        JSValue visibleVal = JS_GetPropertyStr(ctx, opts, "visible");
        if (!JS_IsUndefined(visibleVal))
            node->setVisible(JS_ToBool(ctx, visibleVal) != 0);
        JS_FreeValue(ctx, visibleVal);

        // Position
        double x = qjsbind::get_prop_number(ctx, opts, "x", 0);
        double y = qjsbind::get_prop_number(ctx, opts, "y", 0);
        double z = qjsbind::get_prop_number(ctx, opts, "z", 0);
        node->setPosition((float)x, (float)y, (float)z);

        // Scale — uniform number or per-axis [x, y, z] array (missing array
        // entries default to 1). Non-uniform scale renders correctly: the
        // renderer transforms normals by the model's inverse-transpose
        // (uNormalMat), not the raw model 3x3.
        JSValue scaleVal = JS_GetPropertyStr(ctx, opts, "scale");
        if (!JS_IsUndefined(scaleVal)) {
            if (JS_IsArray(scaleVal)) {
                double s3[3] = {1, 1, 1};
                for (uint32_t i = 0; i < 3; ++i) {
                    JSValue e = JS_GetPropertyUint32(ctx, scaleVal, i);
                    if (!JS_IsUndefined(e)) JS_ToFloat64(ctx, &s3[i], e);
                    JS_FreeValue(ctx, e);
                }
                node->setScale((float)s3[0], (float)s3[1], (float)s3[2]);
            } else {
                double s = 1;
                JS_ToFloat64(ctx, &s, scaleVal);
                node->setScale((float)s, (float)s, (float)s);
            }
        }
        JS_FreeValue(ctx, scaleVal);

        // Rotation (Euler degrees for convenience)
        JSValue rxVal = JS_GetPropertyStr(ctx, opts, "rx");
        JSValue ryVal = JS_GetPropertyStr(ctx, opts, "ry");
        JSValue rzVal = JS_GetPropertyStr(ctx, opts, "rz");
        if (!JS_IsUndefined(rxVal) || !JS_IsUndefined(ryVal) || !JS_IsUndefined(rzVal)) {
            double rx = 0, ry = 0, rz = 0;
            if (!JS_IsUndefined(rxVal)) JS_ToFloat64(ctx, &rx, rxVal);
            if (!JS_IsUndefined(ryVal)) JS_ToFloat64(ctx, &ry, ryVal);
            if (!JS_IsUndefined(rzVal)) JS_ToFloat64(ctx, &rz, rzVal);
            float toRad = 3.14159265f / 180.0f;
            node->setRotationEuler((float)rx * toRad, (float)ry * toRad, (float)rz * toRad);
        }
        JS_FreeValue(ctx, rxVal);
        JS_FreeValue(ctx, ryVal);
        JS_FreeValue(ctx, rzVal);

        // Color
        JSValue colorVal = JS_GetPropertyStr(ctx, opts, "color");
        if (JS_IsString(colorVal)) {
            uint8_t r, g2, b, a;
            if (parseColor(jsStr(ctx, colorVal), r, g2, b, a)) {
                node->setColor(r / 255.0f, g2 / 255.0f, b / 255.0f, a / 255.0f);
            }
        } else if (JS_IsArray(colorVal)) {
            double cr = 1, cg = 1, cb = 1, ca = 1;
            JSValue e0 = JS_GetPropertyUint32(ctx, colorVal, 0);
            JSValue e1 = JS_GetPropertyUint32(ctx, colorVal, 1);
            JSValue e2 = JS_GetPropertyUint32(ctx, colorVal, 2);
            JSValue e3 = JS_GetPropertyUint32(ctx, colorVal, 3);
            JS_ToFloat64(ctx, &cr, e0);
            JS_ToFloat64(ctx, &cg, e1);
            JS_ToFloat64(ctx, &cb, e2);
            if (!JS_IsUndefined(e3)) JS_ToFloat64(ctx, &ca, e3);
            node->setColor((float)cr, (float)cg, (float)cb, (float)ca);
            JS_FreeValue(ctx, e0);
            JS_FreeValue(ctx, e1);
            JS_FreeValue(ctx, e2);
            JS_FreeValue(ctx, e3);
        }
        JS_FreeValue(ctx, colorVal);

        // Emissive intensity (scalar multiplier against emissiveColor)
        double emissive = qjsbind::get_prop_number(ctx, opts, "emissive", 0.0);
        node->setEmissive((float)emissive);

        // Emissive color (defaults to baseColor if unspecified — mimics
        // glTF "emissiveFactor applied to base" for single-field ergonomics).
        JSValue emColVal = JS_GetPropertyStr(ctx, opts, "emissiveColor");
        if (JS_IsString(emColVal)) {
            uint8_t er, eg, eb, ea;
            if (parseColor(jsStr(ctx, emColVal), er, eg, eb, ea))
                node->setEmissiveColor(er/255.0f, eg/255.0f, eb/255.0f);
        } else if (JS_IsArray(emColVal)) {
            double er = 1, eg = 1, eb = 1;
            JSValue e0 = JS_GetPropertyUint32(ctx, emColVal, 0);
            JSValue e1 = JS_GetPropertyUint32(ctx, emColVal, 1);
            JSValue e2 = JS_GetPropertyUint32(ctx, emColVal, 2);
            JS_ToFloat64(ctx, &er, e0);
            JS_ToFloat64(ctx, &eg, e1);
            JS_ToFloat64(ctx, &eb, e2);
            node->setEmissiveColor((float)er, (float)eg, (float)eb);
            JS_FreeValue(ctx, e0);
            JS_FreeValue(ctx, e1);
            JS_FreeValue(ctx, e2);
        } else if (emissive > 0.0) {
            // Default: reuse baseColor so `{color:'#ff0', emissive:2}` glows yellow.
            const float* c = node->color();
            node->setEmissiveColor(c[0], c[1], c[2]);
        }
        JS_FreeValue(ctx, emColVal);

        // PBR material params (glTF metallic/roughness workflow).
        JSValue matVal = JS_GetPropertyStr(ctx, opts, "material");
        auto applyMat = [&](JSValueConst obj) {
            JSValue mv = JS_GetPropertyStr(ctx, obj, "metallic");
            if (!JS_IsUndefined(mv)) node->setMetallic((float)jsNum(ctx, mv));
            JS_FreeValue(ctx, mv);
            JSValue rv = JS_GetPropertyStr(ctx, obj, "roughness");
            if (!JS_IsUndefined(rv)) node->setRoughness((float)jsNum(ctx, rv));
            JS_FreeValue(ctx, rv);
        };
        if (JS_IsObject(matVal)) applyMat(matVal);
        // Flat shortcuts: {metallic:0.9, roughness:0.2}
        applyMat(opts);
        JS_FreeValue(ctx, matVal);

        // Unlit flag — skip lighting for this mesh (output baseColor only).
        JSValue unlitVal = JS_GetPropertyStr(ctx, opts, "unlit");
        if (!JS_IsUndefined(unlitVal)) {
            node->setUnlit(JS_ToBool(ctx, unlitVal) == 1);
        }
        JS_FreeValue(ctx, unlitVal);

        JSValue tsVal = JS_GetPropertyStr(ctx, opts, "twoSided");
        if (!JS_IsUndefined(tsVal)) node->setTwoSided(JS_ToBool(ctx, tsVal) == 1);
        JS_FreeValue(ctx, tsVal);

        JSValue ssVal = JS_GetPropertyStr(ctx, opts, "subsurface");
        if (!JS_IsUndefined(ssVal)) {
            double s = 0;
            JS_ToFloat64(ctx, &s, ssVal);
            node->setSubsurface((float)s);
        }
        JS_FreeValue(ctx, ssVal);

        JSValue acVal = JS_GetPropertyStr(ctx, opts, "alphaCutoff");
        if (!JS_IsUndefined(acVal)) {
            double s = 0;
            JS_ToFloat64(ctx, &s, acVal);
            node->setAlphaCutoff((float)s);
        }
        JS_FreeValue(ctx, acVal);

        // Whether the per-vertex color stream tints albedo. Default (unset)
        // auto-tints iff a color buffer is present. Pass false to keep a
        // color buffer solely as the wind-bend channel (vertex color R) —
        // foliage sways yet keeps its material/textured colour instead of
        // being washed by the bend gradient, so no stripVertexColors hack.
        JSValue vctVal = JS_GetPropertyStr(ctx, opts, "vertexColorTint");
        if (!JS_IsUndefined(vctVal)) node->setVertexColorTint(JS_ToBool(ctx, vctVal) == 1);
        JS_FreeValue(ctx, vctVal);

        // Draw mode — 'lines' switches to GL_LINES (indices = endpoint pairs)
        // and flips the node to unlit + non-shadow-casting. Default 'triangles'.
        // Set before castsShadow/unlit so explicit overrides win.
        JSValue dmVal = JS_GetPropertyStr(ctx, opts, "drawMode");
        if (JS_IsString(dmVal)) {
            std::string s = jsStr(ctx, dmVal);
            if (s == "lines" || s == "line")
                node->setDrawMode(scene::MeshNode::DrawMode::Lines);
            else
                node->setDrawMode(scene::MeshNode::DrawMode::Triangles);
        }
        JS_FreeValue(ctx, dmVal);

        JSValue lwVal = JS_GetPropertyStr(ctx, opts, "lineWidth");
        if (!JS_IsUndefined(lwVal))
            node->setLineWidth((float)jsNum(ctx, lwVal));
        JS_FreeValue(ctx, lwVal);

        // Wind sway opt-in. Accepts a boolean (true → 1.0) or a [0,1] scalar.
        // Per-vertex bend is sourced from vertex-color R (matches Mesh.leafCard
        // output); this multiplier gates the whole mesh.
        JSValue wmVal = JS_GetPropertyStr(ctx, opts, "wind");
        if (!JS_IsUndefined(wmVal)) {
            if (JS_IsBool(wmVal)) {
                node->setWindMask(JS_ToBool(ctx, wmVal) == 1 ? 1.0f : 0.0f);
            } else {
                double s = 0;
                JS_ToFloat64(ctx, &s, wmVal);
                node->setWindMask((float)s);
            }
        }
        JS_FreeValue(ctx, wmVal);

        JSValue csVal = JS_GetPropertyStr(ctx, opts, "castsShadow");
        if (!JS_IsUndefined(csVal)) node->setCastsShadow(JS_ToBool(ctx, csVal) == 1);
        JS_FreeValue(ctx, csVal);
        JSValue rsVal = JS_GetPropertyStr(ctx, opts, "receivesShadow");
        if (!JS_IsUndefined(rsVal)) node->setReceivesShadow(JS_ToBool(ctx, rsVal) == 1);
        JS_FreeValue(ctx, rsVal);

        // Depth bias
        JSValue dbVal = JS_GetPropertyStr(ctx, opts, "depthBias");
        if (!JS_IsUndefined(dbVal)) {
            if (JS_IsArray(dbVal)) {
                double f = 0, u = 0;
                JSValue e0 = JS_GetPropertyUint32(ctx, dbVal, 0);
                JSValue e1 = JS_GetPropertyUint32(ctx, dbVal, 1);
                JS_ToFloat64(ctx, &f, e0);
                JS_ToFloat64(ctx, &u, e1);
                node->setDepthBias((float)f, (float)u);
                JS_FreeValue(ctx, e0);
                JS_FreeValue(ctx, e1);
            } else {
                double u = 0;
                JS_ToFloat64(ctx, &u, dbVal);
                node->setDepthBias(0.0f, (float)u);
            }
        }
        JS_FreeValue(ctx, dbVal);

        // Mesh data
        bromesh::MeshData meshData;
        bool hasRawData = false;

        bool transfer = qjsbind::get_prop_bool(ctx, opts, "transfer", false);

        auto tryKey = [&](const char* key) -> bool {
            JSValue v = JS_GetPropertyStr(ctx, opts, key);
            bool took = false;
            if (!JS_IsUndefined(v) && MeshBindings::getMeshData(ctx, v)) {
                if (transfer) {
                    if (auto taken = MeshBindings::takeMeshData(ctx, v)) {
                        meshData = std::move(*taken);
                        took = true;
                    }
                } else {
                    meshData = *MeshBindings::getMeshData(ctx, v);
                    took = true;
                }
            }
            JS_FreeValue(ctx, v);
            return took;
        };

        if (tryKey("mesh")) hasRawData = true;
        else if (tryKey("data")) hasRawData = true;

        // Check for raw vertex data (positions + indices arrays)
        if (!hasRawData) {
            std::vector<float> positions, normals, colors, uvs, tangents;
            std::vector<uint32_t> indices;
            if (jsReadFloatArray(ctx, opts, "positions", positions) &&
                jsReadUint32Array(ctx, opts, "indices", indices)) {
                meshData.positions = std::move(positions);
                meshData.indices = std::move(indices);
                if (jsReadFloatArray(ctx, opts, "normals", normals)) {
                    meshData.normals = std::move(normals);
                }
                if (jsReadFloatArray(ctx, opts, "colors", colors)) {
                    meshData.colors = std::move(colors);
                }
                // UVs and tangents, which MeshData has always carried and this
                // path has never filled. mesh.vert declares both (aUV, and
                // aTangent at location 4) and passes them to the user chunk, so
                // a shader written against them silently read zeros for every
                // mesh built from raw arrays — the geometry looked right and the
                // material was wrong, which is the hardest kind of gap to see.
                // Loaded meshes came in through a different path and were fine,
                // so nothing in the engine's own content ever exercised it.
                if (jsReadFloatArray(ctx, opts, "uvs", uvs)) {
                    meshData.uvs = std::move(uvs);
                }
                if (jsReadFloatArray(ctx, opts, "tangents", tangents)) {
                    meshData.tangents = std::move(tangents);
                }
                if (meshData.normals.empty() &&
                    qjsbind::get_prop_bool(ctx, opts, "recomputeNormals", false))
                    bromesh::computeNormals(meshData);
                hasRawData = true;
            }
        }

        if (!hasRawData) {
            std::string meshType = qjsbind::get_prop_string(ctx, opts, "mesh", "box");

            if (meshType == "box") {
                double hw = qjsbind::get_prop_number(ctx, opts, "halfW", 0.5);
                double hh = qjsbind::get_prop_number(ctx, opts, "halfH", 0.5);
                double hd = qjsbind::get_prop_number(ctx, opts, "halfD", 0.5);
                meshData = bromesh::box((float)hw, (float)hh, (float)hd);
            } else if (meshType == "sphere") {
                double radius = qjsbind::get_prop_number(ctx, opts, "radius", 0.5);
                int segments = (int)qjsbind::get_prop_number(ctx, opts, "segments", 16);
                int rings = (int)qjsbind::get_prop_number(ctx, opts, "rings", 12);
                meshData = bromesh::sphere((float)radius, segments, rings);
            } else if (meshType == "cylinder") {
                double radius = qjsbind::get_prop_number(ctx, opts, "radius", 0.5);
                double halfH = qjsbind::get_prop_number(ctx, opts, "halfHeight", 0.5);
                int segments = (int)qjsbind::get_prop_number(ctx, opts, "segments", 16);
                meshData = bromesh::cylinder((float)radius, (float)halfH, segments);
            } else if (meshType == "capsule") {
                double radius = qjsbind::get_prop_number(ctx, opts, "radius", 0.5);
                double halfH = qjsbind::get_prop_number(ctx, opts, "halfHeight", 0.5);
                int segments = (int)qjsbind::get_prop_number(ctx, opts, "segments", 16);
                int rings = (int)qjsbind::get_prop_number(ctx, opts, "rings", 8);
                meshData = bromesh::capsule((float)radius, (float)halfH, segments, rings);
            } else if (meshType == "plane") {
                double hw = qjsbind::get_prop_number(ctx, opts, "halfW", 5.0);
                double hd = qjsbind::get_prop_number(ctx, opts, "halfD", 5.0);
                int sx = (int)qjsbind::get_prop_number(ctx, opts, "subdivX", 1);
                int sz = (int)qjsbind::get_prop_number(ctx, opts, "subdivZ", 1);
                meshData = bromesh::plane((float)hw, (float)hd, sx, sz);
            } else if (meshType == "torus") {
                double major = qjsbind::get_prop_number(ctx, opts, "majorRadius", 1.0);
                double minor = qjsbind::get_prop_number(ctx, opts, "minorRadius", 0.3);
                int majSeg = (int)qjsbind::get_prop_number(ctx, opts, "majorSegments", 24);
                int minSeg = (int)qjsbind::get_prop_number(ctx, opts, "minorSegments", 12);
                meshData = bromesh::torus((float)major, (float)minor, majSeg, minSeg);
            }
        }

        node->setMesh(std::move(meshData));
    }

    // Optional texture maps: each is { width, height, data: Uint8Array(rgba8) }.
    // Keys: texture (baseColor), normalTexture, metallicRoughnessTexture,
    //       occlusionTexture, emissiveTexture.
    {
        auto applyTex = [&](const char* key, void (scene::MeshNode::*setter)(int, int, const uint8_t*)) {
            JSValue tex = JS_GetPropertyStr(ctx, opts, key);
            if (JS_IsObject(tex)) {
                int w = (int)qjsbind::get_prop_number(ctx, tex, "width",  0);
                int h = (int)qjsbind::get_prop_number(ctx, tex, "height", 0);
                JSValue dataVal = JS_GetPropertyStr(ctx, tex, "data");
                size_t bytes = 0;
                size_t off = 0, len = 0;
                JSValue ab = JS_GetTypedArrayBuffer(ctx, dataVal, &off, &len, nullptr);
                if (!JS_IsException(ab)) {
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
        applyTex("texture",                  &scene::MeshNode::setBaseColorTexture);
        applyTex("normalTexture",            &scene::MeshNode::setNormalTexture);
        applyTex("metallicRoughnessTexture", &scene::MeshNode::setMetallicRoughnessTexture);
        applyTex("occlusionTexture",         &scene::MeshNode::setOcclusionTexture);
        applyTex("emissiveTexture",          &scene::MeshNode::setEmissiveTexture);
    }
}

// createMesh({mesh, color, name, x, y, z, ...})
JSValue js_sg_createMesh(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;

    auto* node = g->createMesh();
    g->root()->addChild(node);

    if (argc > 0 && JS_IsObject(argv[0]))
        applyMeshNodeOptions(ctx, node, argv[0]);

    return wrapNode(ctx, node, g);
}

// createSkinnedMesh({mesh|data, skin, skinningMatrices?, ...material opts})
// GPU-skinned mesh node. Accepts the full createMesh option surface plus:
//   skin              (required) SkinData — per-vertex weights/indices, e.g.
//                     Mesh.loadGLTF().skins[i] or Rig.autoRig().skin. Must
//                     cover exactly the mesh's vertex count.
//   skinningMatrices  (optional) Float32Array of boneCount * 16 floats —
//                     initial palette in computeSkinningMatrices layout
//                     (world(bone) * inverseBind, column-major). Defaults to
//                     identity = bind pose.
// Drive per frame with node.setSkinningMatrices(pose.computeSkinningMatrices(skel)).
JSValue js_sg_createSkinnedMesh(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "createSkinnedMesh requires an options object with mesh + skin");

    auto* node = g->createSkinnedMesh();
    g->root()->addChild(node);
    applyMeshNodeOptions(ctx, node, argv[0]);

    JSValue skinVal = JS_GetPropertyStr(ctx, argv[0], "skin");
    bromesh::SkinData* sd = RiggingBindings::getSkinData(ctx, skinVal);
    JS_FreeValue(ctx, skinVal);
    if (!sd) {
        g->destroyNode(node);
        return JS_ThrowTypeError(ctx,
            "createSkinnedMesh: opts.skin must be a SkinData");
    }
    if (!node->setSkin(*sd)) {
        g->destroyNode(node);
        return JS_ThrowTypeError(ctx,
            "createSkinnedMesh: skin rejected (bone count 0 or > 256, or "
            "weight/index streams malformed)");
    }
    if (!node->skinReady()) {
        g->destroyNode(node);
        return JS_ThrowTypeError(ctx,
            "createSkinnedMesh: skin vertex count does not match the mesh");
    }

    std::vector<float> palette;
    if (jsReadFloatArray(ctx, argv[0], "skinningMatrices", palette) &&
        palette.size() >= 16) {
        node->setSkinningMatrices(palette.data(), palette.size() / 16);
    }

    return wrapNode(ctx, node, g);
}

// setSkinningMatrices(Float32Array) — upload the bone palette for a skinned
// mesh node. Layout matches Pose.computeSkinningMatrices output: count * 16
// floats, column-major mat4 per bone (world(bone) * inverseBind(bone)).
// Matrices beyond the node's boneCount are ignored. Zero mesh re-upload —
// this is the per-frame hot path of the CPU-rig → GPU-skin pipeline.
JSValue js_node_setSkinningMatrices(JSContext* ctx, JSValueConst this_val,
                                           int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node() || w->node()->type() != scene::SceneNode::Type::Mesh)
        return JS_ThrowTypeError(ctx, "setSkinningMatrices: not a mesh node");
    auto* sm = static_cast<scene::MeshNode*>(w->node())->asSkinnedMesh();
    if (!sm)
        return JS_ThrowTypeError(ctx,
            "setSkinningMatrices: node is not a skinned mesh (use createSkinnedMesh)");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "setSkinningMatrices: missing matrix array");

    size_t offset = 0, byteLen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[0], &offset, &byteLen, &bpe);
    if (JS_IsException(abuf)) {
        JS_FreeValue(ctx, abuf);
        return JS_ThrowTypeError(ctx,
            "setSkinningMatrices: expected a Float32Array");
    }
    size_t abufLen = 0;
    uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    if (!raw || bpe != sizeof(float))
        return JS_ThrowTypeError(ctx,
            "setSkinningMatrices: expected a Float32Array");

    const float* data = reinterpret_cast<const float*>(raw + offset);
    int n = sm->setSkinningMatrices(data, byteLen / sizeof(float) / 16);
    return JS_NewInt32(ctx, n);
}

// createInstancedMesh({mesh, instances|instancesFromTransforms, color, ...})
// Mirrors createMesh's material + texture surface but renders N copies of
// `mesh` in a single draw via hardware instancing. Per-instance state is
// either:
//   - `instances`: Float32Array of 16*count floats (canonical layout —
//     4x3 affine transform rows + RGBA tint), or
//   - `instancesFromTransforms`: Float32Array of 9*count floats
//     (px py pz, qx qy qz qw, scale, variantIndex), converted internally.
// Apply a `scatter` descriptor to an InstancedMeshNode, switching it into GPU
// foliage-scatter mode. `scatterVal` carries the packed per-segment buffer
// (segments: Float32Array, 8 floats/segment) plus the placement params, as
// produced by `world.emitScatterSegments(opts)` spread with the same opts. See
// InstancedMeshNode::setScatterSegments / shaders/foliage_scatter.vert.
static bool applyScatter(JSContext* ctx, scene::InstancedMeshNode* node,
                         JSValueConst scatterVal) {
    if (!JS_IsObject(scatterVal)) return false;
    std::vector<float> seg, instSeg;
    if (!jsReadFloatArray(ctx, scatterVal, "segments", seg) || seg.empty())
        return false;
    if (!jsReadFloatArray(ctx, scatterVal, "instSeg", instSeg) || instSeg.empty())
        return false;
    size_t segCount = seg.size() / 8;
    if (segCount == 0) return false;

    scene::InstancedMeshNode::ScatterParams p;
    p.seed          = (uint32_t)qjsbind::get_prop_number(ctx, scatterVal, "seed", 0);
    p.upBias        = (float)qjsbind::get_prop_number(ctx, scatterVal, "upBias", 0.5);
    p.tiltJitter    = (float)qjsbind::get_prop_number(ctx, scatterVal, "tiltJitter", 0.3);
    p.rollJitter    = (float)qjsbind::get_prop_number(ctx, scatterVal, "rollJitter", 0.2);
    p.baseScale     = (float)qjsbind::get_prop_number(ctx, scatterVal, "baseScale", 1.0);
    p.scaleJitter   = (float)qjsbind::get_prop_number(ctx, scatterVal, "scaleJitter", 0.2);
    p.scaleByRadius = (float)qjsbind::get_prop_number(ctx, scatterVal, "scaleByRadius", 0.0);
    p.refRadius     = (float)qjsbind::get_prop_number(ctx, scatterVal, "maxRadius", 0.05);
    p.densityFalloff= (float)qjsbind::get_prop_number(ctx, scatterVal, "densityFalloff", 0.0);

    // Bounds: prefer explicit boundsMin/boundsMax (emit supplies them); else
    // derive from the segment endpoints and pad for leaf reach.
    float bmin[3], bmax[3];
    std::vector<float> vmin, vmax;
    bool haveB = jsReadFloatArray(ctx, scatterVal, "boundsMin", vmin) && vmin.size() >= 3 &&
                 jsReadFloatArray(ctx, scatterVal, "boundsMax", vmax) && vmax.size() >= 3;
    if (haveB) {
        for (int i = 0; i < 3; ++i) { bmin[i] = vmin[i]; bmax[i] = vmax[i]; }
    } else {
        bmin[0] = bmin[1] = bmin[2] =  1e30f;
        bmax[0] = bmax[1] = bmax[2] = -1e30f;
        for (size_t s = 0; s < segCount; ++s) {
            const float* r = seg.data() + s * 8;
            float from[3] = { r[0], r[1], r[2] };
            float to[3]   = { r[0] + r[4], r[1] + r[5], r[2] + r[6] };  // from + dir
            for (int i = 0; i < 3; ++i) {
                bmin[i] = std::min({ bmin[i], from[i], to[i] });
                bmax[i] = std::max({ bmax[i], from[i], to[i] });
            }
        }
        float pad = p.baseScale * (1.0f + p.scaleJitter) * 0.3f;  // leaf reach
        for (int i = 0; i < 3; ++i) { bmin[i] -= pad; bmax[i] += pad; }
    }
    node->setScatterSegments(seg.data(), segCount, instSeg.data(), instSeg.size(),
                             p, bmin, bmax);
    return true;
}

// Apply a `tube` option / setTubeSegments argument: a compact per-segment
// buffer (8 floats each: [from.xyz, radiusFrom, to.xyz, radiusTo]) that the
// tube VS expands into tapered stem geometry. Produced by
// `world.emitBranchTubes(opts)`. See InstancedMeshNode::setTubeSegments /
// shaders/branch_tube.vert.
static bool applyTube(JSContext* ctx, scene::InstancedMeshNode* node,
                      JSValueConst tubeVal) {
    if (!JS_IsObject(tubeVal)) return false;
    std::vector<float> seg;
    if (!jsReadFloatArray(ctx, tubeVal, "segments", seg) || seg.empty())
        return false;
    size_t segCount = seg.size() / 8;
    if (segCount == 0) return false;

    int   sides       = (int)qjsbind::get_prop_number(ctx, tubeVal, "sides", 6);
    float radiusScale = (float)qjsbind::get_prop_number(ctx, tubeVal, "radiusScale", 1.0);

    // Bounds: prefer explicit boundsMin/boundsMax (emit supplies them); else
    // derive from the segment endpoints, padded by the max ring radius.
    float bmin[3], bmax[3];
    std::vector<float> vmin, vmax;
    bool haveB = jsReadFloatArray(ctx, tubeVal, "boundsMin", vmin) && vmin.size() >= 3 &&
                 jsReadFloatArray(ctx, tubeVal, "boundsMax", vmax) && vmax.size() >= 3;
    if (haveB) {
        for (int i = 0; i < 3; ++i) { bmin[i] = vmin[i]; bmax[i] = vmax[i]; }
    } else {
        bmin[0] = bmin[1] = bmin[2] =  1e30f;
        bmax[0] = bmax[1] = bmax[2] = -1e30f;
        float maxR = 0.0f;
        for (size_t s = 0; s < segCount; ++s) {
            const float* r = seg.data() + s * 8;
            float from[3] = { r[0], r[1], r[2] };
            float to[3]   = { r[4], r[5], r[6] };
            maxR = std::max({ maxR, r[3], r[7] });
            for (int i = 0; i < 3; ++i) {
                bmin[i] = std::min({ bmin[i], from[i], to[i] });
                bmax[i] = std::max({ bmax[i], from[i], to[i] });
            }
        }
        float pad = maxR * radiusScale;
        for (int i = 0; i < 3; ++i) { bmin[i] -= pad; bmax[i] += pad; }
    }
    node->setTubeSegments(seg.data(), segCount, sides, radiusScale, bmin, bmax);
    return true;
}

JSValue js_sg_createInstancedMesh(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;

    auto* node = g->createInstancedMesh();
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

        // Color
        JSValue colorVal = JS_GetPropertyStr(ctx, opts, "color");
        if (JS_IsString(colorVal)) {
            uint8_t r, g2, b, a;
            if (parseColor(jsStr(ctx, colorVal), r, g2, b, a)) {
                node->setColor(r/255.0f, g2/255.0f, b/255.0f, a/255.0f);
            }
        } else if (JS_IsArray(colorVal)) {
            double cr = 1, cg = 1, cb = 1, ca = 1;
            JSValue e0 = JS_GetPropertyUint32(ctx, colorVal, 0);
            JSValue e1 = JS_GetPropertyUint32(ctx, colorVal, 1);
            JSValue e2 = JS_GetPropertyUint32(ctx, colorVal, 2);
            JSValue e3 = JS_GetPropertyUint32(ctx, colorVal, 3);
            JS_ToFloat64(ctx, &cr, e0); JS_ToFloat64(ctx, &cg, e1);
            JS_ToFloat64(ctx, &cb, e2);
            if (!JS_IsUndefined(e3)) JS_ToFloat64(ctx, &ca, e3);
            node->setColor((float)cr, (float)cg, (float)cb, (float)ca);
            JS_FreeValue(ctx, e0); JS_FreeValue(ctx, e1);
            JS_FreeValue(ctx, e2); JS_FreeValue(ctx, e3);
        }
        JS_FreeValue(ctx, colorVal);

        double emissive = qjsbind::get_prop_number(ctx, opts, "emissive", 0.0);
        node->setEmissive((float)emissive);

        JSValue emColVal = JS_GetPropertyStr(ctx, opts, "emissiveColor");
        if (JS_IsString(emColVal)) {
            uint8_t er, eg, eb, ea;
            if (parseColor(jsStr(ctx, emColVal), er, eg, eb, ea))
                node->setEmissiveColor(er/255.0f, eg/255.0f, eb/255.0f);
        } else if (JS_IsArray(emColVal)) {
            double er = 1, eg = 1, eb = 1;
            JSValue e0 = JS_GetPropertyUint32(ctx, emColVal, 0);
            JSValue e1 = JS_GetPropertyUint32(ctx, emColVal, 1);
            JSValue e2 = JS_GetPropertyUint32(ctx, emColVal, 2);
            JS_ToFloat64(ctx, &er, e0); JS_ToFloat64(ctx, &eg, e1); JS_ToFloat64(ctx, &eb, e2);
            node->setEmissiveColor((float)er, (float)eg, (float)eb);
            JS_FreeValue(ctx, e0); JS_FreeValue(ctx, e1); JS_FreeValue(ctx, e2);
        } else if (emissive > 0.0) {
            const float* c = node->color();
            node->setEmissiveColor(c[0], c[1], c[2]);
        }
        JS_FreeValue(ctx, emColVal);

        JSValue mv = JS_GetPropertyStr(ctx, opts, "metallic");
        if (!JS_IsUndefined(mv)) node->setMetallic((float)jsNum(ctx, mv));
        JS_FreeValue(ctx, mv);
        JSValue rv = JS_GetPropertyStr(ctx, opts, "roughness");
        if (!JS_IsUndefined(rv)) node->setRoughness((float)jsNum(ctx, rv));
        JS_FreeValue(ctx, rv);

        JSValue unlitVal = JS_GetPropertyStr(ctx, opts, "unlit");
        if (!JS_IsUndefined(unlitVal)) node->setUnlit(JS_ToBool(ctx, unlitVal) == 1);
        JS_FreeValue(ctx, unlitVal);

        JSValue acV = JS_GetPropertyStr(ctx, opts, "alphaCutoff");
        if (!JS_IsUndefined(acV)) node->setAlphaCutoff((float)jsNum(ctx, acV));
        JS_FreeValue(ctx, acV);

        JSValue vctV = JS_GetPropertyStr(ctx, opts, "vertexColorTint");
        if (!JS_IsUndefined(vctV)) node->setVertexColorTint(JS_ToBool(ctx, vctV) == 1);
        JS_FreeValue(ctx, vctV);

        JSValue dsV = JS_GetPropertyStr(ctx, opts, "doubleSided");
        if (!JS_IsUndefined(dsV)) node->setDoubleSided(JS_ToBool(ctx, dsV) == 1);
        JS_FreeValue(ctx, dsV);

        JSValue csVal = JS_GetPropertyStr(ctx, opts, "castsShadow");
        if (!JS_IsUndefined(csVal)) node->setCastsShadow(JS_ToBool(ctx, csVal) == 1);
        JS_FreeValue(ctx, csVal);
        JSValue rsVal = JS_GetPropertyStr(ctx, opts, "receivesShadow");
        if (!JS_IsUndefined(rsVal)) node->setReceivesShadow(JS_ToBool(ctx, rsVal) == 1);
        JS_FreeValue(ctx, rsVal);

        // Mesh source — accept a Mesh handle from MeshBindings.
        bool transfer = qjsbind::get_prop_bool(ctx, opts, "transfer", false);
        JSValue meshVal = JS_GetPropertyStr(ctx, opts, "mesh");
        if (!JS_IsUndefined(meshVal) && MeshBindings::getMeshData(ctx, meshVal)) {
            if (transfer) {
                if (auto taken = MeshBindings::takeMeshData(ctx, meshVal))
                    node->setMesh(std::move(*taken));
            } else {
                node->setMesh(*MeshBindings::getMeshData(ctx, meshVal));
            }
        }
        JS_FreeValue(ctx, meshVal);

        // Instance buffer.
        std::vector<float> raw;
        if (jsReadFloatArray(ctx, opts, "instances", raw)) {
            size_t count = raw.size() / 16;
            node->setInstances(raw.data(), count);
        } else if (jsReadFloatArray(ctx, opts, "instancesFromTransforms", raw)) {
            size_t count = raw.size() / 9;
            node->setInstancesFromPosQuatScale(raw.data(), count);
        }

        // Texture maps — same shape as createMesh.
        auto applyTex = [&](const char* key, void (scene::InstancedMeshNode::*setter)(int, int, const uint8_t*)) {
            JSValue tex = JS_GetPropertyStr(ctx, opts, key);
            if (JS_IsObject(tex)) {
                int w = (int)qjsbind::get_prop_number(ctx, tex, "width",  0);
                int h = (int)qjsbind::get_prop_number(ctx, tex, "height", 0);
                JSValue dataVal = JS_GetPropertyStr(ctx, tex, "data");
                size_t bytes = 0, off = 0, len = 0;
                JSValue ab = JS_GetTypedArrayBuffer(ctx, dataVal, &off, &len, nullptr);
                if (!JS_IsException(ab)) {
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
        applyTex("texture",                  &scene::InstancedMeshNode::setBaseColorTexture);
        applyTex("normalTexture",            &scene::InstancedMeshNode::setNormalTexture);
        applyTex("metallicRoughnessTexture", &scene::InstancedMeshNode::setMetallicRoughnessTexture);
        applyTex("occlusionTexture",         &scene::InstancedMeshNode::setOcclusionTexture);
        applyTex("emissiveTexture",          &scene::InstancedMeshNode::setEmissiveTexture);

        JSValue acVal = JS_GetPropertyStr(ctx, opts, "atlasCols");
        JSValue arVal = JS_GetPropertyStr(ctx, opts, "atlasRows");
        if (!JS_IsUndefined(acVal) || !JS_IsUndefined(arVal)) {
            int ac = JS_IsUndefined(acVal) ? 1 : (int)jsNum(ctx, acVal);
            int ar = JS_IsUndefined(arVal) ? 1 : (int)jsNum(ctx, arVal);
            node->setAtlasGrid(ac, ar);
        }
        JS_FreeValue(ctx, acVal);
        JS_FreeValue(ctx, arVal);

        // Static batching: collapse all instances into one merged draw. Kills
        // the per-instance GPU cost for high counts of tiny meshes. Opt-in —
        // see InstancedMeshNode::setStaticBatch.
        JSValue sbVal = JS_GetPropertyStr(ctx, opts, "staticBatch");
        if (!JS_IsUndefined(sbVal)) node->setStaticBatch(JS_ToBool(ctx, sbVal) == 1);
        JS_FreeValue(ctx, sbVal);

        // GPU foliage scatter: leaves synthesised in the VS from a per-segment
        // buffer (mutually exclusive with `instances`).
        JSValue scatVal = JS_GetPropertyStr(ctx, opts, "scatter");
        if (JS_IsObject(scatVal)) applyScatter(ctx, node, scatVal);
        JS_FreeValue(ctx, scatVal);

        // GPU procedural branch tubes: tapered stem geometry synthesised in the
        // VS from a per-segment buffer (needs no template mesh, mutually
        // exclusive with `instances` and `scatter`).
        JSValue tubeVal = JS_GetPropertyStr(ctx, opts, "tube");
        if (JS_IsObject(tubeVal)) applyTube(ctx, node, tubeVal);
        JS_FreeValue(ctx, tubeVal);
    }

    return wrapNode(ctx, node, g);
}

// Live update of an instanced node's tube segments (the skeleton grew). Same
// descriptor shape as the `tube` create option.
JSValue js_node_setTubeSegments(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node() || w->node()->type() != scene::SceneNode::Type::InstancedMesh)
        return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    applyTube(ctx, static_cast<scene::InstancedMeshNode*>(w->node()), argv[0]);
    return JS_UNDEFINED;
}

// Live update of an instanced node's scatter segments (the sim grew). Same
// descriptor shape as the `scatter` create option.
JSValue js_node_setScatterSegments(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node() || w->node()->type() != scene::SceneNode::Type::InstancedMesh)
        return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    applyScatter(ctx, static_cast<scene::InstancedMeshNode*>(w->node()), argv[0]);
    return JS_UNDEFINED;
}

// Per-node setters for InstancedMeshNode — setMesh / setInstances /
// setInstancesFromTransforms / updateInstance.
JSValue js_node_setInstancedMesh(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node() || w->node()->type() != scene::SceneNode::Type::InstancedMesh) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    auto* node = static_cast<scene::InstancedMeshNode*>(w->node());
    if (MeshBindings::getMeshData(ctx, argv[0])) {
        node->setMesh(*MeshBindings::getMeshData(ctx, argv[0]));
    }
    return JS_UNDEFINED;
}

JSValue js_node_setInstances(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node() || w->node()->type() != scene::SceneNode::Type::InstancedMesh) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    size_t off = 0, len = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &len, nullptr);
    if (JS_IsException(ab)) { JS_FreeValue(ctx, ab); return JS_UNDEFINED; }
    size_t bytes = 0;
    uint8_t* base = JS_GetArrayBuffer(ctx, &bytes, ab);
    if (base) {
        const float* data = reinterpret_cast<const float*>(base + off);
        size_t count = (len / sizeof(float)) / 16;
        static_cast<scene::InstancedMeshNode*>(w->node())->setInstances(data, count);
    }
    JS_FreeValue(ctx, ab);
    return JS_UNDEFINED;
}

JSValue js_node_setInstancesFromTransforms(JSContext* ctx, JSValueConst this_val,
                                                  int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node() || w->node()->type() != scene::SceneNode::Type::InstancedMesh) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    size_t off = 0, len = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &len, nullptr);
    if (JS_IsException(ab)) { JS_FreeValue(ctx, ab); return JS_UNDEFINED; }
    size_t bytes = 0;
    uint8_t* base = JS_GetArrayBuffer(ctx, &bytes, ab);
    if (base) {
        const float* data = reinterpret_cast<const float*>(base + off);
        size_t count = (len / sizeof(float)) / 9;
        static_cast<scene::InstancedMeshNode*>(w->node())->setInstancesFromPosQuatScale(data, count);
    }
    JS_FreeValue(ctx, ab);
    return JS_UNDEFINED;
}

JSValue js_node_updateInstance(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node() || w->node()->type() != scene::SceneNode::Type::InstancedMesh) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, argv[0]);
    size_t off = 0, len = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[1], &off, &len, nullptr);
    if (JS_IsException(ab)) { JS_FreeValue(ctx, ab); return JS_UNDEFINED; }
    size_t bytes = 0;
    uint8_t* base = JS_GetArrayBuffer(ctx, &bytes, ab);
    if (base && len >= sizeof(float) * 16 && idx >= 0) {
        const float* data = reinterpret_cast<const float*>(base + off);
        static_cast<scene::InstancedMeshNode*>(w->node())->updateInstance((size_t)idx, data);
    }
    JS_FreeValue(ctx, ab);
    return JS_UNDEFINED;
}

JSValue js_node_setAtlasGrid(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node() || w->node()->type() != scene::SceneNode::Type::InstancedMesh) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t cols = 1, rows = 1;
    JS_ToInt32(ctx, &cols, argv[0]);
    JS_ToInt32(ctx, &rows, argv[1]);
    static_cast<scene::InstancedMeshNode*>(w->node())->setAtlasGrid(cols, rows);
    return JS_UNDEFINED;
}

JSValue js_node_setAlphaCutoff(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node() || w->node()->type() != scene::SceneNode::Type::InstancedMesh) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    static_cast<scene::InstancedMeshNode*>(w->node())->setAlphaCutoff((float)jsNum(ctx, argv[0]));
    return JS_UNDEFINED;
}

JSValue js_node_setDoubleSided(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node() || w->node()->type() != scene::SceneNode::Type::InstancedMesh) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    static_cast<scene::InstancedMeshNode*>(w->node())->setDoubleSided(JS_ToBool(ctx, argv[0]) == 1);
    return JS_UNDEFINED;
}

} // namespace bro::js

#endif  // BRO_WITH_3D
