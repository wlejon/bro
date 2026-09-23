// The SceneNode members the bronze port left behind: per-type accessors
// (alphaCutoff, doubleSided, interior, priority, emissiveColor, radius,
// fillColor / strokeColor / strokeWidth, autoSync, pixelsPerUnit, billboard,
// nearClipDist, frameIndex) and the instanced-mesh operations (updateInstance,
// setInstancedMesh, setAtlasGrid, setScatterSegments, setTubeSegments) plus
// PhysicsNode.syncToPhysics. Hand-registered beside the generated scene
// natives (registerSceneExtraNatives, chained from registerSceneNatives), the
// same way registerPhysicsNatives adds setMotionType; js/scene_extras.js is
// the wrapper over them, entered right after js/scene.js.
//
// Every accessor answers for the node types it applies to and is a no-op /
// undefined elsewhere, which is what the old per-type `.prop` bindings did
// and what an app that reads `node.radius` off a mesh expects (undefined,
// not a throw).

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"

#include <bromesh/mesh_data.h>
#include <bromesh/manipulation/normals.h>
#include <bromesh/api.h>

#include "util/string_utils.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace bro::bronze_host;
namespace canvas = bro::canvas;

namespace {

scene::MeshNode* meshOf(void* self) {
    auto* n = nodeOf(self);
    return (n && n->type() == scene::SceneNode::Type::Mesh) ? static_cast<scene::MeshNode*>(n) : nullptr;
}

scene::InstancedMeshNode* instancedOf(void* self) {
    auto* n = nodeOf(self);
    return (n && n->type() == scene::SceneNode::Type::InstancedMesh)
               ? static_cast<scene::InstancedMeshNode*>(n) : nullptr;
}

scene::ReflectionProbeNode* probeOf(void* self) {
    auto* n = nodeOf(self);
    return (n && n->type() == scene::SceneNode::Type::ReflectionProbe)
               ? static_cast<scene::ReflectionProbeNode*>(n) : nullptr;
}

scene::ShapeNode* shapeOf(void* self) {
    auto* n = nodeOf(self);
    return (n && n->type() == scene::SceneNode::Type::Shape) ? static_cast<scene::ShapeNode*>(n) : nullptr;
}

scene::PhysicsNode* physicsOf(void* self) {
    auto* n = nodeOf(self);
    return (n && n->type() == scene::SceneNode::Type::Physics) ? static_cast<scene::PhysicsNode*>(n) : nullptr;
}

scene::SpriteNode* spriteOf(void* self) {
    auto* n = nodeOf(self);
    return (n && n->type() == scene::SceneNode::Type::Sprite) ? static_cast<scene::SpriteNode*>(n) : nullptr;
}

// The same serialization canvas 2D's fillStyle getter answers (HTML's
// serialization of a color): "#rrggbb" when opaque, "rgba(r, g, b, a)"
// otherwise, the channels taken to 8 bits first.
const char* cssOf(bromath::Color c) {
    auto byte = [](float v) {
        return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    const std::string s = bro::util::serializeCanvasColor(byte(c.r), byte(c.g), byte(c.b), byte(c.a));
    return strResult(s.c_str());
}

bool colorOf(const char* css, bromath::Color& out) {
    uint8_t r, g, b, a;
    if (!css || !canvas::parseCSSColor(css, r, g, b, a)) return false;
    out = bromath::cfromColor8(bromath::Color8(r, g, b, a));
    return true;
}

// A Mesh handle (bromesh's HostClass, ev::handleData) or a plain
// { positions, indices, normals?, uvs?, colors?, tangents? } — the shape
// updateMesh takes (native_scene_shaders.cpp).
bool meshDataOf(uint64_t meshBits, bromesh::MeshData& out, const char* who) {
    Value meshVal = ev::fromBits(meshBits);
    if (!ev::isObject(meshVal)) {
        ev::throwTypeError(std::string(who) + ": expected a Mesh or { positions, indices, ... }");
        return false;
    }
    if (const auto* md = bromesh::api::meshDataOf(meshVal)) {
        out = *md;
        return true;
    }
    if (!readFloatVector(ev::getProperty(meshVal, "positions"), out.positions) ||
        !readU32Vector(ev::getProperty(meshVal, "indices"), out.indices)) {
        ev::throwTypeError(std::string(who) + ": positions (Float32Array) and indices (Uint32Array) are required");
        return false;
    }
    readFloatVector(ev::getProperty(meshVal, "normals"), out.normals);
    readFloatVector(ev::getProperty(meshVal, "uvs"), out.uvs);
    readFloatVector(ev::getProperty(meshVal, "colors"), out.colors);
    readFloatVector(ev::getProperty(meshVal, "tangents"), out.tangents);
    if (out.normals.empty() && !out.positions.empty()) bromesh::computeNormals(out);
    return true;
}

}  // namespace

extern "C" {

// ---- alphaCutoff (Mesh, InstancedMesh) ------------------------------------
double bro_scene_SceneNode_alphaCutoff_get(void* self) {
    if (auto* m = meshOf(self)) return m->alphaCutoff();
    if (auto* im = instancedOf(self)) return im->alphaCutoff();
    return 0.0;
}
void bro_scene_SceneNode_alphaCutoff_set(void* self, double v) {
    if (auto* m = meshOf(self)) m->setAlphaCutoff(static_cast<float>(v));
    else if (auto* im = instancedOf(self)) im->setAlphaCutoff(static_cast<float>(v));
}

// ---- doubleSided (InstancedMesh) ------------------------------------------
bool bro_scene_SceneNode_doubleSided_get(void* self) {
    auto* im = instancedOf(self);
    return im ? im->doubleSided() : false;
}
void bro_scene_SceneNode_doubleSided_set(void* self, bool v) {
    if (auto* im = instancedOf(self)) im->setDoubleSided(v);
}

// ---- interior / priority (ReflectionProbe) --------------------------------
double bro_scene_SceneNode_interior_get(void* self) {
    auto* p = probeOf(self);
    return p ? p->interior() : 0.0;
}
void bro_scene_SceneNode_interior_set(void* self, double v) {
    if (auto* p = probeOf(self)) p->setInterior(static_cast<float>(v));
}
int32_t bro_scene_SceneNode_priority_get(void* self) {
    auto* p = probeOf(self);
    return p ? p->priority() : 0;
}
void bro_scene_SceneNode_priority_set(void* self, int32_t v) {
    if (auto* p = probeOf(self)) p->setPriority(v);
}

// ---- emissiveColor (Mesh, InstancedMesh): [r, g, b] in 0..1 ---------------
void bro_scene_SceneNode_emissiveColor_get(void* self, bronze_native_buffer* out) {
    const float* c = nullptr;
    if (auto* m = meshOf(self)) c = m->emissiveColor();
    else if (auto* im = instancedOf(self)) c = im->emissiveColor();
    if (!c) { transferBuffer(std::vector<double>{}, out); return; }
    transferBuffer(std::vector<double>{c[0], c[1], c[2]}, out);
}
void bro_scene_SceneNode_emissiveColor_set(void* self, const double* v, uint32_t len) {
    const float* cur = nullptr;
    scene::MeshNode* m = meshOf(self);
    scene::InstancedMeshNode* im = m ? nullptr : instancedOf(self);
    if (m) cur = m->emissiveColor();
    else if (im) cur = im->emissiveColor();
    if (!cur) return;
    float c[3] = {cur[0], cur[1], cur[2]};
    for (uint32_t i = 0; i < 3 && i < len; ++i) c[i] = static_cast<float>(v[i]);
    if (m) m->setEmissiveColor(c[0], c[1], c[2]);
    else im->setEmissiveColor(c[0], c[1], c[2]);
}
// Emissive is a plain 0..1 triple, not a linearized bromath::Color: a CSS
// string maps channel/255 straight, as the array form and the old binding did.
void bro_scene_SceneNode_emissiveColorCss_set(void* self, const char* css) {
    uint8_t r, g, b, a;
    if (!css || !canvas::parseCSSColor(css, r, g, b, a)) return;
    const float fr = r / 255.0f, fg = g / 255.0f, fb = b / 255.0f;
    if (auto* m = meshOf(self)) m->setEmissiveColor(fr, fg, fb);
    else if (auto* im = instancedOf(self)) im->setEmissiveColor(fr, fg, fb);
}

// ---- nearClipDist (Mesh, InstancedMesh) -----------------------------------
double bro_scene_SceneNode_nearClipDist_get(void* self) {
    if (auto* m = meshOf(self)) return m->nearClipDist();
    if (auto* im = instancedOf(self)) return im->nearClipDist();
    return 0.0;
}
void bro_scene_SceneNode_nearClipDist_set(void* self, double v) {
    if (auto* m = meshOf(self)) m->setNearClipDist(static_cast<float>(v));
    else if (auto* im = instancedOf(self)) im->setNearClipDist(static_cast<float>(v));
}

// ---- Shape: radius, fillColor, strokeColor, strokeWidth -------------------
double bro_scene_SceneNode_radius_get(void* self) {
    auto* s = shapeOf(self);
    return s ? s->radius() : 0.0;
}
void bro_scene_SceneNode_radius_set(void* self, double v) {
    if (auto* s = shapeOf(self)) s->setRadius(static_cast<float>(v));
}
const char* bro_scene_SceneNode_fillColor_get(void* self) {
    auto* s = shapeOf(self);
    return s ? cssOf(s->fillColor()) : "";
}
void bro_scene_SceneNode_fillColor_set(void* self, const char* css) {
    bromath::Color c;
    if (auto* s = shapeOf(self); s && colorOf(css, c)) s->setFillColor(c);
}
const char* bro_scene_SceneNode_strokeColor_get(void* self) {
    auto* s = shapeOf(self);
    return s ? cssOf(s->strokeColor()) : "";
}
void bro_scene_SceneNode_strokeColor_set(void* self, const char* css) {
    bromath::Color c;
    if (auto* s = shapeOf(self); s && colorOf(css, c)) s->setStrokeColor(c);
}
double bro_scene_SceneNode_strokeWidth_get(void* self) {
    auto* s = shapeOf(self);
    return s ? s->strokeWidth() : 0.0;
}
void bro_scene_SceneNode_strokeWidth_set(void* self, double v) {
    if (auto* s = shapeOf(self)) {
        s->setStrokeWidth(static_cast<float>(v));
        if (v <= 0.0) s->setHasStroke(false);
    }
}

// ---- Physics: autoSync, pixelsPerUnit, syncToPhysics ----------------------
bool bro_scene_SceneNode_autoSync_get(void* self) {
    auto* p = physicsOf(self);
    return p ? p->autoSync() : false;
}
void bro_scene_SceneNode_autoSync_set(void* self, bool v) {
    if (auto* p = physicsOf(self)) p->setAutoSync(v);
}
double bro_scene_SceneNode_pixelsPerUnit_get(void* self) {
    auto* p = physicsOf(self);
    return p ? p->pixelsPerUnit() : 0.0;
}
void bro_scene_SceneNode_pixelsPerUnit_set(void* self, double v) {
    if (auto* p = physicsOf(self)) p->setPixelsPerUnit(static_cast<float>(v));
}
void bro_scene_SceneNode_syncToPhysics(void* self) {
    auto* cell = nodeCellOf(self);
    auto* p = physicsOf(self);
    if (!cell || !p) return;
    scene::SceneGraph* g = cell->graph();
    if (g && g->physicsWorld()) p->syncToPhysics(g->physicsWorld());
}

// ---- billboard (any node; drawn by Shape/Sprite/Html) ---------------------
const char* bro_scene_SceneNode_billboard_get(void* self) {
    auto* n = nodeOf(self);
    if (!n) return "";
    return n->billboardMode() == scene::SceneNode::BillboardMode::YLock ? "ylock" : "full";
}
void bro_scene_SceneNode_billboard_set(void* self, const char* mode) {
    auto* n = nodeOf(self);
    if (!n || !mode) return;
    std::string s(mode);
    if (s == "ylock" || s == "yLock" || s == "y-lock") {
        n->setBillboardMode(scene::SceneNode::BillboardMode::YLock);
    } else {
        n->setBillboardMode(scene::SceneNode::BillboardMode::Full);
    }
}

// ---- Sprite: frameIndex ---------------------------------------------------
int32_t bro_scene_SceneNode_frameIndex_get(void* self) {
    auto* s = spriteOf(self);
    return s ? s->frameIndex() : 0;
}
void bro_scene_SceneNode_frameIndex_set(void* self, int32_t v) {
    if (auto* s = spriteOf(self)) s->setFrameIndex(v);
}

// ---- anchorX / anchorY (Shape, Sprite) ------------------------------------
double bro_scene_SceneNode_anchorX_get(void* self) {
    if (auto* s = shapeOf(self)) return s->anchorX();
    if (auto* s = spriteOf(self)) return s->anchorX();
    return 0.5;
}
double bro_scene_SceneNode_anchorY_get(void* self) {
    if (auto* s = shapeOf(self)) return s->anchorY();
    if (auto* s = spriteOf(self)) return s->anchorY();
    return 0.5;
}
void bro_scene_SceneNode_anchor_set(void* self, double ax, double ay) {
    if (auto* s = shapeOf(self)) s->setAnchor(static_cast<float>(ax), static_cast<float>(ay));
    else if (auto* s = spriteOf(self)) s->setAnchor(static_cast<float>(ax), static_cast<float>(ay));
}

// ---- cornerRadius (Shape) -------------------------------------------------
double bro_scene_SceneNode_cornerRadius_get(void* self) {
    auto* s = shapeOf(self);
    return s ? s->cornerRadius() : 0.0;
}
void bro_scene_SceneNode_cornerRadius_set(void* self, double v) {
    if (auto* s = shapeOf(self)) s->setCornerRadius(static_cast<float>(v));
}

// ---- pxPerUnit (Html) -----------------------------------------------------
double bro_scene_SceneNode_pxPerUnit_get(void* self) {
    auto* n = nodeOf(self);
    return (n && n->type() == scene::SceneNode::Type::Html) ? static_cast<scene::HtmlNode*>(n)->pxPerUnit() : 0.0;
}
void bro_scene_SceneNode_pxPerUnit_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Html && v > 0.0) {
        static_cast<scene::HtmlNode*>(n)->setPxPerUnit(static_cast<float>(v));
    }
}

// ---- bodyId (Physics): the Jolt body id, -1 with no body ------------------
double bro_scene_SceneNode_bodyId_get(void* self) {
    auto* p = physicsOf(self);
    if (!p || !p->hasBody()) return -1.0;
    return static_cast<double>(p->bodyId().GetIndexAndSequenceNumber());
}

// ---- atlasCols / atlasRows / staticBatch (InstancedMesh) ------------------
int32_t bro_scene_SceneNode_atlasCols_get(void* self) {
    auto* im = instancedOf(self);
    return im ? im->atlasCols() : 0;
}
int32_t bro_scene_SceneNode_atlasRows_get(void* self) {
    auto* im = instancedOf(self);
    return im ? im->atlasRows() : 0;
}
bool bro_scene_SceneNode_staticBatch_get(void* self) {
    auto* im = instancedOf(self);
    return im ? im->staticBatch() : false;
}
void bro_scene_SceneNode_staticBatch_set(void* self, bool v) {
    if (auto* im = instancedOf(self)) im->setStaticBatch(v);
}

// ---- InstancedMesh operations ---------------------------------------------
void bro_scene_SceneNode_updateInstance(void* self, int32_t index, const float* data, uint32_t len) {
    auto* im = instancedOf(self);
    if (!im) { ev::throwTypeError("updateInstance: node is not an InstancedMesh"); return; }
    if (index < 0 || len < 16) {
        ev::throwTypeError("updateInstance: expected (index >= 0, 16 floats)");
        return;
    }
    im->updateInstance(static_cast<size_t>(index), data);
}

void bro_scene_SceneNode_setInstancedMesh(void* self, uint64_t meshBits) {
    auto* im = instancedOf(self);
    if (!im) { ev::throwTypeError("setInstancedMesh: node is not an InstancedMesh"); return; }
    bromesh::MeshData md;
    if (!meshDataOf(meshBits, md, "setInstancedMesh")) return;
    im->setMesh(std::move(md));
}

void bro_scene_SceneNode_setAtlasGrid(void* self, int32_t cols, int32_t rows) {
    if (auto* im = instancedOf(self)) im->setAtlasGrid(cols, rows);
}

// params: [seed, upBias, tiltJitter, rollJitter, baseScale, scaleJitter,
// scaleByRadius, maxRadius, densityFalloff]; bounds: [minX,minY,minZ,
// maxX,maxY,maxZ] or empty to derive from the segment endpoints, padded for
// leaf reach — the rule the old applyScatter used.
void bro_scene_SceneNode_setScatterSegments(void* self, const float* seg, uint32_t segLen,
                                            const float* instSeg, uint32_t instLen,
                                            const double* params, uint32_t paramLen,
                                            const float* bounds, uint32_t boundsLen) {
    auto* im = instancedOf(self);
    if (!im) { ev::throwTypeError("setScatterSegments: node is not an InstancedMesh"); return; }
    const size_t segCount = segLen / 8;
    if (segCount == 0 || instLen == 0) return;

    scene::InstancedMeshNode::ScatterParams p;
    auto at = [&](uint32_t i, double d) { return i < paramLen ? params[i] : d; };
    p.seed           = static_cast<uint32_t>(at(0, 0));
    p.upBias         = static_cast<float>(at(1, 0.5));
    p.tiltJitter     = static_cast<float>(at(2, 0.3));
    p.rollJitter     = static_cast<float>(at(3, 0.2));
    p.baseScale      = static_cast<float>(at(4, 1.0));
    p.scaleJitter    = static_cast<float>(at(5, 0.2));
    p.scaleByRadius  = static_cast<float>(at(6, 0.0));
    p.refRadius      = static_cast<float>(at(7, 0.05));
    p.densityFalloff = static_cast<float>(at(8, 0.0));

    float bmin[3], bmax[3];
    if (boundsLen >= 6) {
        for (int i = 0; i < 3; ++i) { bmin[i] = bounds[i]; bmax[i] = bounds[3 + i]; }
    } else {
        bmin[0] = bmin[1] = bmin[2] = 1e30f;
        bmax[0] = bmax[1] = bmax[2] = -1e30f;
        for (size_t s = 0; s < segCount; ++s) {
            const float* r = seg + s * 8;
            const float from[3] = {r[0], r[1], r[2]};
            const float to[3] = {r[0] + r[4], r[1] + r[5], r[2] + r[6]};  // from + dir
            for (int i = 0; i < 3; ++i) {
                bmin[i] = std::min({bmin[i], from[i], to[i]});
                bmax[i] = std::max({bmax[i], from[i], to[i]});
            }
        }
        const float pad = p.baseScale * (1.0f + p.scaleJitter) * 0.3f;  // leaf reach
        for (int i = 0; i < 3; ++i) { bmin[i] -= pad; bmax[i] += pad; }
    }
    im->setScatterSegments(seg, segCount, instSeg, instLen, p, bmin, bmax);
}

// seg: segCount*8 floats [from.xyz, rFrom, to.xyz, rTo]; bounds as above or
// empty to derive from the endpoints padded by the widest ring.
void bro_scene_SceneNode_setTubeSegments(void* self, const float* seg, uint32_t segLen,
                                         int32_t sides, double radiusScale,
                                         const float* bounds, uint32_t boundsLen) {
    auto* im = instancedOf(self);
    if (!im) { ev::throwTypeError("setTubeSegments: node is not an InstancedMesh"); return; }
    const size_t segCount = segLen / 8;
    if (segCount == 0) return;
    if (sides < 3) sides = 6;
    const float rs = static_cast<float>(radiusScale > 0.0 ? radiusScale : 1.0);

    float bmin[3], bmax[3];
    if (boundsLen >= 6) {
        for (int i = 0; i < 3; ++i) { bmin[i] = bounds[i]; bmax[i] = bounds[3 + i]; }
    } else {
        bmin[0] = bmin[1] = bmin[2] = 1e30f;
        bmax[0] = bmax[1] = bmax[2] = -1e30f;
        float maxR = 0.0f;
        for (size_t s = 0; s < segCount; ++s) {
            const float* r = seg + s * 8;
            const float from[3] = {r[0], r[1], r[2]};
            const float to[3] = {r[4], r[5], r[6]};
            maxR = std::max({maxR, r[3], r[7]});
            for (int i = 0; i < 3; ++i) {
                bmin[i] = std::min({bmin[i], from[i], to[i]});
                bmax[i] = std::max({bmax[i], from[i], to[i]});
            }
        }
        const float pad = maxR * rs;
        for (int i = 0; i < 3; ++i) { bmin[i] -= pad; bmax[i] += pad; }
    }
    im->setTubeSegments(seg, segCount, sides, rs, bmin, bmax);
}

bool bro_scene_SceneNode_isScatter(void* self) {
    auto* im = instancedOf(self);
    return im ? im->isScatter() : false;
}
bool bro_scene_SceneNode_isTube(void* self) {
    auto* im = instancedOf(self);
    return im ? im->isTube() : false;
}

}  // extern "C"

namespace bro::bronze_host {

bool registerSceneExtraNatives(std::string* error) {
    using natives::fn;
    static const char* const N = "__bro_native.scene.SceneNode";
    return fn("__bro_native.scene.SceneNode_alphaCutoff_get", (void*)&bro_scene_SceneNode_alphaCutoff_get, "f64", {N}, error) &&
           fn("__bro_native.scene.SceneNode_alphaCutoff_set", (void*)&bro_scene_SceneNode_alphaCutoff_set, "void", {N, "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_doubleSided_get", (void*)&bro_scene_SceneNode_doubleSided_get, "bool", {N}, error) &&
           fn("__bro_native.scene.SceneNode_doubleSided_set", (void*)&bro_scene_SceneNode_doubleSided_set, "void", {N, "bool"}, error) &&
           fn("__bro_native.scene.SceneNode_interior_get", (void*)&bro_scene_SceneNode_interior_get, "f64", {N}, error) &&
           fn("__bro_native.scene.SceneNode_interior_set", (void*)&bro_scene_SceneNode_interior_set, "void", {N, "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_priority_get", (void*)&bro_scene_SceneNode_priority_get, "i32", {N}, error) &&
           fn("__bro_native.scene.SceneNode_priority_set", (void*)&bro_scene_SceneNode_priority_set, "void", {N, "i32"}, error) &&
           fn("__bro_native.scene.SceneNode_emissiveColor_get", (void*)&bro_scene_SceneNode_emissiveColor_get, "f64[]", {N}, error) &&
           fn("__bro_native.scene.SceneNode_emissiveColor_set", (void*)&bro_scene_SceneNode_emissiveColor_set, "void", {N, "f64[]"}, error) &&
           fn("__bro_native.scene.SceneNode_emissiveColorCss_set", (void*)&bro_scene_SceneNode_emissiveColorCss_set, "void", {N, "str"}, error) &&
           fn("__bro_native.scene.SceneNode_nearClipDist_get", (void*)&bro_scene_SceneNode_nearClipDist_get, "f64", {N}, error) &&
           fn("__bro_native.scene.SceneNode_nearClipDist_set", (void*)&bro_scene_SceneNode_nearClipDist_set, "void", {N, "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_radius_get", (void*)&bro_scene_SceneNode_radius_get, "f64", {N}, error) &&
           fn("__bro_native.scene.SceneNode_radius_set", (void*)&bro_scene_SceneNode_radius_set, "void", {N, "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_fillColor_get", (void*)&bro_scene_SceneNode_fillColor_get, "str", {N}, error) &&
           fn("__bro_native.scene.SceneNode_fillColor_set", (void*)&bro_scene_SceneNode_fillColor_set, "void", {N, "str"}, error) &&
           fn("__bro_native.scene.SceneNode_strokeColor_get", (void*)&bro_scene_SceneNode_strokeColor_get, "str", {N}, error) &&
           fn("__bro_native.scene.SceneNode_strokeColor_set", (void*)&bro_scene_SceneNode_strokeColor_set, "void", {N, "str"}, error) &&
           fn("__bro_native.scene.SceneNode_strokeWidth_get", (void*)&bro_scene_SceneNode_strokeWidth_get, "f64", {N}, error) &&
           fn("__bro_native.scene.SceneNode_strokeWidth_set", (void*)&bro_scene_SceneNode_strokeWidth_set, "void", {N, "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_autoSync_get", (void*)&bro_scene_SceneNode_autoSync_get, "bool", {N}, error) &&
           fn("__bro_native.scene.SceneNode_autoSync_set", (void*)&bro_scene_SceneNode_autoSync_set, "void", {N, "bool"}, error) &&
           fn("__bro_native.scene.SceneNode_pixelsPerUnit_get", (void*)&bro_scene_SceneNode_pixelsPerUnit_get, "f64", {N}, error) &&
           fn("__bro_native.scene.SceneNode_pixelsPerUnit_set", (void*)&bro_scene_SceneNode_pixelsPerUnit_set, "void", {N, "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_syncToPhysics", (void*)&bro_scene_SceneNode_syncToPhysics, "void", {N}, error) &&
           fn("__bro_native.scene.SceneNode_billboard_get", (void*)&bro_scene_SceneNode_billboard_get, "str", {N}, error) &&
           fn("__bro_native.scene.SceneNode_billboard_set", (void*)&bro_scene_SceneNode_billboard_set, "void", {N, "str"}, error) &&
           fn("__bro_native.scene.SceneNode_frameIndex_get", (void*)&bro_scene_SceneNode_frameIndex_get, "i32", {N}, error) &&
           fn("__bro_native.scene.SceneNode_frameIndex_set", (void*)&bro_scene_SceneNode_frameIndex_set, "void", {N, "i32"}, error) &&
           fn("__bro_native.scene.SceneNode_updateInstance", (void*)&bro_scene_SceneNode_updateInstance, "void", {N, "i32", "f32[]"}, error) &&
           fn("__bro_native.scene.SceneNode_setInstancedMesh", (void*)&bro_scene_SceneNode_setInstancedMesh, "void", {N, "dynamic"}, error) &&
           fn("__bro_native.scene.SceneNode_setAtlasGrid", (void*)&bro_scene_SceneNode_setAtlasGrid, "void", {N, "i32", "i32"}, error) &&
           fn("__bro_native.scene.SceneNode_setScatterSegments", (void*)&bro_scene_SceneNode_setScatterSegments, "void", {N, "f32[]", "f32[]", "f64[]", "f32[]"}, error) &&
           fn("__bro_native.scene.SceneNode_setTubeSegments", (void*)&bro_scene_SceneNode_setTubeSegments, "void", {N, "f32[]", "i32", "f64", "f32[]"}, error) &&
           fn("__bro_native.scene.SceneNode_isScatter", (void*)&bro_scene_SceneNode_isScatter, "bool", {N}, error) &&
           fn("__bro_native.scene.SceneNode_isTube", (void*)&bro_scene_SceneNode_isTube, "bool", {N}, error) &&
           fn("__bro_native.scene.SceneNode_anchorX_get", (void*)&bro_scene_SceneNode_anchorX_get, "f64", {N}, error) &&
           fn("__bro_native.scene.SceneNode_anchorY_get", (void*)&bro_scene_SceneNode_anchorY_get, "f64", {N}, error) &&
           fn("__bro_native.scene.SceneNode_anchor_set", (void*)&bro_scene_SceneNode_anchor_set, "void", {N, "f64", "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_cornerRadius_get", (void*)&bro_scene_SceneNode_cornerRadius_get, "f64", {N}, error) &&
           fn("__bro_native.scene.SceneNode_cornerRadius_set", (void*)&bro_scene_SceneNode_cornerRadius_set, "void", {N, "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_pxPerUnit_get", (void*)&bro_scene_SceneNode_pxPerUnit_get, "f64", {N}, error) &&
           fn("__bro_native.scene.SceneNode_pxPerUnit_set", (void*)&bro_scene_SceneNode_pxPerUnit_set, "void", {N, "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_bodyId_get", (void*)&bro_scene_SceneNode_bodyId_get, "f64", {N}, error) &&
           fn("__bro_native.scene.SceneNode_atlasCols_get", (void*)&bro_scene_SceneNode_atlasCols_get, "i32", {N}, error) &&
           fn("__bro_native.scene.SceneNode_atlasRows_get", (void*)&bro_scene_SceneNode_atlasRows_get, "i32", {N}, error) &&
           fn("__bro_native.scene.SceneNode_staticBatch_get", (void*)&bro_scene_SceneNode_staticBatch_get, "bool", {N}, error) &&
           fn("__bro_native.scene.SceneNode_staticBatch_set", (void*)&bro_scene_SceneNode_staticBatch_set, "void", {N, "bool"}, error);
}

}  // namespace bro::bronze_host
