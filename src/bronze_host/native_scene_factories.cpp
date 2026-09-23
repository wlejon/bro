// native_scene_factories.cpp — SceneGraph node creation factories.

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
#include "natives/scene/native_scene_decl.h"
#include "scene/particles3d_node.h"
#include "scene/physics_node.h"
#include "scene/animation_player.h"
#include "util/log.h"

#include <bromesh/primitives/primitives.h>
#include <bromesh/manipulation/normals.h>
#include <bromesh/api.h>
#include <json.hpp>

namespace bro::bronze_host {
// A particle colour from JSON: [r,g,b(,a)] or "#rrggbb", white otherwise.
// A free function rather than a lambda inside createParticles3D because MSVC
// gives a captureless lambda in an extern "C" function a C-linkage invoker,
// which may not return a C++ class (C2526).
bromath::Color particleColorFromJson(const nlohmann::json& v) {
    if (v.is_array() && v.size() >= 3) {
        float a = v.size() >= 4 ? v[3].get<float>() : 1.0f;
        return {v[0].get<float>(), v[1].get<float>(), v[2].get<float>(), a};
    }
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        if (!s.empty() && s[0] == '#' && s.size() == 7) {
            int r = std::stoi(s.substr(1, 2), nullptr, 16);
            int g = std::stoi(s.substr(3, 2), nullptr, 16);
            int b = std::stoi(s.substr(5, 2), nullptr, 16);
            return {r / 255.0f, g / 255.0f, b / 255.0f, 1.0f};
        }
    }
    return {1.0f, 1.0f, 1.0f, 1.0f};
}

// The material half of the createMesh / createSkinnedMesh option surface
// (see SceneGraph.createMesh in docs/scene-api.js). Transform keys (x/y/z,
// scale, rx/ry/rz, name, visible) are applied by the JS wrapper's
// applyNodeOpts through the node attributes.
void applyMeshMaterialOpts(scene::MeshNode* node, Value opts) {
    Value colorVal = ev::getProperty(opts, "color");
    float cr = 1, cg = 1, cb = 1, ca = 1;
    if (parseColorValue(colorVal, cr, cg, cb, ca)) {
        node->setColor(cr, cg, cb, ca);
    }

    // PBR params: nested {material:{metallic, roughness}} and the flat
    // shortcuts, the flat ones winning.
    auto applyMat = [&](Value obj) {
        Value metVal = ev::getProperty(obj, "metallic");
        if (ev::isNumber(metVal)) node->setMetallic(static_cast<float>(ev::toDouble(metVal)));
        Value roughVal = ev::getProperty(obj, "roughness");
        if (ev::isNumber(roughVal)) node->setRoughness(static_cast<float>(ev::toDouble(roughVal)));
    };
    Value matVal = ev::getProperty(opts, "material");
    if (ev::isObject(matVal)) applyMat(matVal);
    applyMat(opts);

    // Emissive intensity, then its tint: an explicit emissiveColor, else
    // the base colour so `{color:'#ff0', emissive:2}` glows yellow.
    Value emissVal = ev::getProperty(opts, "emissive");
    const float emissive = ev::isNumber(emissVal) ? static_cast<float>(ev::toDouble(emissVal)) : 0.0f;
    if (ev::isNumber(emissVal)) node->setEmissive(emissive);
    Value emColVal = ev::getProperty(opts, "emissiveColor");
    float er = 1, eg = 1, eb = 1, ea = 1;
    if (parseColorValue(emColVal, er, eg, eb, ea)) {
        node->setEmissiveColor(er, eg, eb);
    } else if (emissive > 0.0f) {
        const float* c = node->color();
        node->setEmissiveColor(c[0], c[1], c[2]);
    }

    Value unlitVal = ev::getProperty(opts, "unlit");
    if (!ev::isUndefined(unlitVal)) node->setUnlit(ev::toBool(unlitVal));

    // twoSided (doubleSided is the glTF spelling), subsurface wrap,
    // alpha test, vertex-colour tint.
    Value tsVal = ev::getProperty(opts, "twoSided");
    if (ev::isUndefined(tsVal)) tsVal = ev::getProperty(opts, "doubleSided");
    if (!ev::isUndefined(tsVal)) node->setTwoSided(ev::toBool(tsVal));
    Value ssVal = ev::getProperty(opts, "subsurface");
    if (ev::isNumber(ssVal)) node->setSubsurface(static_cast<float>(ev::toDouble(ssVal)));
    Value acVal = ev::getProperty(opts, "alphaCutoff");
    if (ev::isNumber(acVal)) node->setAlphaCutoff(static_cast<float>(ev::toDouble(acVal)));
    Value vctVal = ev::getProperty(opts, "vertexColorTint");
    if (!ev::isUndefined(vctVal)) node->setVertexColorTint(ev::toBool(vctVal));

    // Draw mode before castsShadow/unlit so explicit overrides win:
    // 'lines' flips the node to unlit + non-shadow-casting.
    Value dmVal = ev::getProperty(opts, "drawMode");
    if (ev::isString(dmVal)) {
        const std::string s = ev::toUtf8(dmVal);
        node->setDrawMode((s == "lines" || s == "line") ? scene::MeshNode::DrawMode::Lines
                                                        : scene::MeshNode::DrawMode::Triangles);
    }
    Value lwVal = ev::getProperty(opts, "lineWidth");
    if (ev::isNumber(lwVal)) node->setLineWidth(static_cast<float>(ev::toDouble(lwVal)));

    // Wind sway opt-in: true → 1.0, or a [0,1] whole-mesh multiplier.
    Value wmVal = ev::getProperty(opts, "wind");
    if (ev::isBool(wmVal)) node->setWindMask(ev::toBool(wmVal) ? 1.0f : 0.0f);
    else if (ev::isNumber(wmVal)) node->setWindMask(static_cast<float>(ev::toDouble(wmVal)));

    Value csVal = ev::getProperty(opts, "castsShadow");
    if (!ev::isUndefined(csVal)) node->setCastsShadow(ev::toBool(csVal));
    Value rsVal = ev::getProperty(opts, "receivesShadow");
    if (!ev::isUndefined(rsVal)) node->setReceivesShadow(ev::toBool(rsVal));

    // Depth bias: [factor, units] or a bare units value.
    Value dbVal = ev::getProperty(opts, "depthBias");
    if (ev::isNumber(dbVal)) {
        node->setDepthBias(0.0f, static_cast<float>(ev::toDouble(dbVal)));
    } else if (ev::isObject(dbVal)) {
        std::vector<float> db;
        if (readFloatVector(dbVal, db) && db.size() >= 2) node->setDepthBias(db[0], db[1]);
    }

    // Texture maps, each { width, height, data: Uint8Array(rgba8) }.
    auto applyTex = [&](const char* key, void (scene::MeshNode::*setter)(int, int, const uint8_t*)) {
        Value tex = ev::getProperty(opts, key);
        if (!ev::isObject(tex)) return;
        Value wV = ev::getProperty(tex, "width");
        Value hV = ev::getProperty(tex, "height");
        if (!ev::isNumber(wV) || !ev::isNumber(hV)) return;
        const int w = static_cast<int>(ev::toDouble(wV));
        const int h = static_cast<int>(ev::toDouble(hV));
        auto info = ev::typedArrayInfo(ev::getProperty(tex, "data"));
        if (info && w > 0 && h > 0 && info.byteLength >= static_cast<size_t>(w) * static_cast<size_t>(h) * 4) {
            (node->*setter)(w, h, info.data);
        }
    };
    applyTex("texture",                  &scene::MeshNode::setBaseColorTexture);
    applyTex("normalTexture",            &scene::MeshNode::setNormalTexture);
    applyTex("metallicRoughnessTexture", &scene::MeshNode::setMetallicRoughnessTexture);
    applyTex("occlusionTexture",         &scene::MeshNode::setOcclusionTexture);
    applyTex("emissiveTexture",          &scene::MeshNode::setEmissiveTexture);
}

// The createInstancedMesh option surface the old binding read: material,
// texture maps, the atlas grid and static batching. Transform / name keys
// are applied by the JS wrapper's applyNodeOpts, the instance buffers by
// setInstances, and scatter / tube by js/scene_extras.js.
void applyInstancedOpts(scene::InstancedMeshNode* node, Value opts) {
    float cr = 1, cg = 1, cb = 1, ca = 1;
    if (parseColorValue(ev::getProperty(opts, "color"), cr, cg, cb, ca)) node->setColor(cr, cg, cb, ca);

    Value emissVal = ev::getProperty(opts, "emissive");
    const float emissive = ev::isNumber(emissVal) ? static_cast<float>(ev::toDouble(emissVal)) : 0.0f;
    if (ev::isNumber(emissVal)) node->setEmissive(emissive);
    float er = 1, eg = 1, eb = 1, ea = 1;
    if (parseColorValue(ev::getProperty(opts, "emissiveColor"), er, eg, eb, ea)) {
        node->setEmissiveColor(er, eg, eb);
    } else if (emissive > 0.0f) {
        const float* c = node->color();
        node->setEmissiveColor(c[0], c[1], c[2]);
    }

    auto num = [&](const char* k, auto&& set) {
        Value v = ev::getProperty(opts, k);
        if (ev::isNumber(v)) set(static_cast<float>(ev::toDouble(v)));
    };
    auto flag = [&](const char* k, auto&& set) {
        Value v = ev::getProperty(opts, k);
        if (!ev::isUndefined(v)) set(ev::toBool(v));
    };
    num("metallic", [&](float v) { node->setMetallic(v); });
    num("roughness", [&](float v) { node->setRoughness(v); });
    num("alphaCutoff", [&](float v) { node->setAlphaCutoff(v); });
    flag("unlit", [&](bool v) { node->setUnlit(v); });
    flag("vertexColorTint", [&](bool v) { node->setVertexColorTint(v); });
    flag("doubleSided", [&](bool v) { node->setDoubleSided(v); });
    flag("castsShadow", [&](bool v) { node->setCastsShadow(v); });
    flag("receivesShadow", [&](bool v) { node->setReceivesShadow(v); });

    auto applyTex = [&](const char* key, void (scene::InstancedMeshNode::*setter)(int, int, const uint8_t*)) {
        Value tex = ev::getProperty(opts, key);
        if (!ev::isObject(tex)) return;
        Value wV = ev::getProperty(tex, "width");
        Value hV = ev::getProperty(tex, "height");
        if (!ev::isNumber(wV) || !ev::isNumber(hV)) return;
        const int w = static_cast<int>(ev::toDouble(wV));
        const int h = static_cast<int>(ev::toDouble(hV));
        auto info = ev::typedArrayInfo(ev::getProperty(tex, "data"));
        if (info && w > 0 && h > 0 && info.byteLength >= static_cast<size_t>(w) * static_cast<size_t>(h) * 4) {
            (node->*setter)(w, h, info.data);
        }
    };
    applyTex("texture",                  &scene::InstancedMeshNode::setBaseColorTexture);
    applyTex("normalTexture",            &scene::InstancedMeshNode::setNormalTexture);
    applyTex("metallicRoughnessTexture", &scene::InstancedMeshNode::setMetallicRoughnessTexture);
    applyTex("occlusionTexture",         &scene::InstancedMeshNode::setOcclusionTexture);
    applyTex("emissiveTexture",          &scene::InstancedMeshNode::setEmissiveTexture);

    // Atlas grid: either key alone defaults the other to 1.
    Value acVal = ev::getProperty(opts, "atlasCols");
    Value arVal = ev::getProperty(opts, "atlasRows");
    if (ev::isNumber(acVal) || ev::isNumber(arVal)) {
        node->setAtlasGrid(ev::isNumber(acVal) ? static_cast<int>(ev::toDouble(acVal)) : 1,
                           ev::isNumber(arVal) ? static_cast<int>(ev::toDouble(arVal)) : 1);
    }

    // Collapse every instance into one merged draw (InstancedMeshNode::setStaticBatch).
    flag("staticBatch", [&](bool v) { node->setStaticBatch(v); });
}

}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;

void* bro_scene_SceneGraph_createMesh(void* self, uint64_t optsBits, uint64_t meshVal) {
    auto* g = graphOf(self);
    if (!g) return nullptr;

    auto* node = g->createMesh();
    if (!node) return nullptr;
    g->root()->addChild(node);

    bromesh::MeshData meshData;
    if (const auto* md = bromesh::api::meshDataOf(bronze::Value{meshVal})) {
        meshData = *md;
    }

    Value opts = ev::fromBits(optsBits);
    if (ev::isObject(opts)) {
        // 1. Raw positions/indices
        Value posVal = ev::getProperty(opts, "positions");
        Value idxVal = ev::getProperty(opts, "indices");
        bool hasRaw = false;
        if (!ev::isUndefined(posVal) && !ev::isUndefined(idxVal)) {
            if (readFloatVector(posVal, meshData.positions) && readU32Vector(idxVal, meshData.indices)) {
                readFloatVector(ev::getProperty(opts, "normals"), meshData.normals);
                readFloatVector(ev::getProperty(opts, "colors"), meshData.colors);
                readFloatVector(ev::getProperty(opts, "uvs"), meshData.uvs);
                readFloatVector(ev::getProperty(opts, "tangents"), meshData.tangents);
                hasRaw = true;
            }
        }

        // 2. Mesh object (`mesh` or its `data` alias) or primitive name
        Value meshProp = ev::getProperty(opts, "mesh");
        if (!hasRaw && meshData.positions.empty()) {
            Value dataProp = ev::getProperty(opts, "data");
            for (Value cand : {meshProp, dataProp}) {
                if (!ev::isObject(cand)) continue;
                if (const auto* md = bromesh::api::meshDataOf(cand)) {
                    meshData = *md;
                    hasRaw = true;
                    break;
                }
            }
        }

        if (!hasRaw && meshData.positions.empty()) {
            std::string meshType = "box";
            if (ev::isString(meshProp)) meshType = ev::toUtf8(meshProp);
            auto getNum = [&](const char* k, float def) -> float {
                Value v = ev::getProperty(opts, k);
                return ev::isNumber(v) ? static_cast<float>(ev::toDouble(v)) : def;
            };
            // bromesh::cylinder/capsule take a HALF height; `halfHeight` is
            // the documented key, `height` the full-extent convenience.
            auto halfHeight = [&]() -> float {
                Value hh = ev::getProperty(opts, "halfHeight");
                if (ev::isNumber(hh)) return static_cast<float>(ev::toDouble(hh));
                Value h = ev::getProperty(opts, "height");
                if (ev::isNumber(h)) return static_cast<float>(ev::toDouble(h)) * 0.5f;
                return 0.5f;
            };
            if (meshType == "sphere") {
                float r = getNum("radius", 0.5f);
                int seg = static_cast<int>(getNum("segments", 16));
                int rings = static_cast<int>(getNum("rings", 12));
                meshData = bromesh::sphere(r, seg, rings);
            } else if (meshType == "cylinder") {
                float r = getNum("radius", 0.5f);
                int seg = static_cast<int>(getNum("segments", 16));
                meshData = bromesh::cylinder(r, halfHeight(), seg);
            } else if (meshType == "capsule") {
                float r = getNum("radius", 0.5f);
                int seg = static_cast<int>(getNum("segments", 16));
                int rings = static_cast<int>(getNum("rings", 8));
                meshData = bromesh::capsule(r, halfHeight(), seg, rings);
            } else if (meshType == "plane") {
                float hw = getNum("halfW", 5.0f);
                float hd = getNum("halfD", 5.0f);
                int sx = static_cast<int>(getNum("subdivX", 1));
                int sz = static_cast<int>(getNum("subdivZ", 1));
                meshData = bromesh::plane(hw, hd, sx, sz);
            } else if (meshType == "torus") {
                float maj = getNum("majorRadius", 1.0f);
                float min = getNum("minorRadius", 0.3f);
                int majSeg = static_cast<int>(getNum("majorSegments", 24));
                int minSeg = static_cast<int>(getNum("minorSegments", 12));
                meshData = bromesh::torus(maj, min, majSeg, minSeg);
            } else {
                float hw = getNum("halfW", 0.5f);
                float hh = getNum("halfH", 0.5f);
                float hd = getNum("halfD", 0.5f);
                meshData = bromesh::box(hw, hh, hd);
            }
        }

        if (meshData.normals.empty() && !meshData.positions.empty()) {
            bromesh::computeNormals(meshData);
        }
        node->setMesh(std::move(meshData));
        applyMeshMaterialOpts(node, opts);
    } else {
        if (meshData.positions.empty()) {
            meshData = bromesh::box(0.5f, 0.5f, 0.5f);
        }
        if (meshData.normals.empty()) {
            bromesh::computeNormals(meshData);
        }
        node->setMesh(std::move(meshData));
    }

    return wrapNode(node, g);
}

void* bro_scene_SceneGraph_createSkinnedMesh(void* self, uint64_t optsBits, uint64_t meshHandle) {
    auto* g = graphOf(self);
    if (!g) return nullptr;

    Value opts = ev::fromBits(optsBits);
    if (!ev::isObject(opts)) {
        ev::throwTypeError("createSkinnedMesh: options object with 'skin' is required");
        return nullptr;
    }
    Value skinVal = ev::getProperty(opts, "skin");
    const auto* sd = ev::isObject(skinVal) ? bromesh::api::skinDataOf(skinVal) : nullptr;
    if (!sd) {
        ev::throwTypeError("createSkinnedMesh: 'skin' option (SkinData) is required");
        return nullptr;
    }

    bromesh::MeshData meshData;
    if (const auto* md = bromesh::api::meshDataOf(bronze::Value{meshHandle})) {
        meshData = *md;
    }

    // 1. Raw positions/indices
    Value posVal = ev::getProperty(opts, "positions");
    Value idxVal = ev::getProperty(opts, "indices");
    bool hasRaw = false;
    if (!ev::isUndefined(posVal) && !ev::isUndefined(idxVal)) {
        if (readFloatVector(posVal, meshData.positions) && readU32Vector(idxVal, meshData.indices)) {
            readFloatVector(ev::getProperty(opts, "normals"), meshData.normals);
            readFloatVector(ev::getProperty(opts, "colors"), meshData.colors);
            readFloatVector(ev::getProperty(opts, "uvs"), meshData.uvs);
            readFloatVector(ev::getProperty(opts, "tangents"), meshData.tangents);
            hasRaw = true;
        }
    }

    // 2. Mesh object (`mesh` or its `data` alias) or primitive name
    Value meshProp = ev::getProperty(opts, "mesh");
    if (!hasRaw && meshData.positions.empty()) {
        Value dataProp = ev::getProperty(opts, "data");
        for (Value cand : {meshProp, dataProp}) {
            if (!ev::isObject(cand)) continue;
            if (const auto* md = bromesh::api::meshDataOf(cand)) {
                meshData = *md;
                hasRaw = true;
                break;
            }
        }
    }

    if (!hasRaw && meshData.positions.empty()) {
        std::string meshType = "box";
        if (ev::isString(meshProp)) meshType = ev::toUtf8(meshProp);
        auto getNum = [&](const char* k, float def) -> float {
            Value v = ev::getProperty(opts, k);
            return ev::isNumber(v) ? static_cast<float>(ev::toDouble(v)) : def;
        };
        if (meshType == "sphere") {
            float r = getNum("radius", 0.5f);
            int seg = static_cast<int>(getNum("segments", 16));
            int rings = static_cast<int>(getNum("rings", 12));
            meshData = bromesh::sphere(r, seg, rings);
        } else if (meshType == "cylinder") {
            float r = getNum("radius", 0.5f);
            float h = getNum("height", 1.0f);
            int seg = static_cast<int>(getNum("segments", 16));
            meshData = bromesh::cylinder(r, h, seg);
        } else if (meshType == "capsule") {
            float r = getNum("radius", 0.5f);
            float h = getNum("height", 1.0f);
            int seg = static_cast<int>(getNum("segments", 16));
            int rings = static_cast<int>(getNum("rings", 8));
            meshData = bromesh::capsule(r, h, seg, rings);
        } else if (meshType == "plane") {
            float hw = getNum("halfW", 5.0f);
            float hd = getNum("halfD", 5.0f);
            int sx = static_cast<int>(getNum("subdivX", 1));
            int sz = static_cast<int>(getNum("subdivZ", 1));
            meshData = bromesh::plane(hw, hd, sx, sz);
        } else if (meshType == "torus") {
            float maj = getNum("majorRadius", 1.0f);
            float min = getNum("minorRadius", 0.3f);
            int majSeg = static_cast<int>(getNum("majorSegments", 24));
            int minSeg = static_cast<int>(getNum("minorSegments", 12));
            meshData = bromesh::torus(maj, min, majSeg, minSeg);
        } else {
            float hw = getNum("halfW", 0.5f);
            float hh = getNum("halfH", 0.5f);
            float hd = getNum("halfD", 0.5f);
            meshData = bromesh::box(hw, hh, hd);
        }
    }

    size_t vertCount = meshData.positions.size() / 3;
    size_t skinVertCount = sd->boneWeights.size() / 4;
    if (skinVertCount != vertCount) {
        ev::throwTypeError("createSkinnedMesh: skin vertex count does not match mesh vertex count");
        return nullptr;
    }

    auto* node = g->createSkinnedMesh();
    g->root()->addChild(node);

    if (meshData.normals.empty() && !meshData.positions.empty()) {
        bromesh::computeNormals(meshData);
    }
    node->setMesh(std::move(meshData));
    node->setSkin(*sd);

    Value skelProp = ev::getProperty(opts, "skeleton");
    if (ev::isObject(skelProp)) {
        if (const auto* skel = bromesh::api::skeletonOf(skelProp)) {
            node->ensurePlayer().setSkeleton(std::make_shared<bromesh::Skeleton>(*skel));
        }
    }

    applyMeshMaterialOpts(node, opts);
    Value nameVal = ev::getProperty(opts, "name");
    if (ev::isString(nameVal)) node->setName(ev::toUtf8(nameVal));

    return wrapNode(node, g);
}

void* bro_scene_SceneGraph_createInstancedMesh(void* self, uint64_t optsBits, uint64_t meshVal) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->createInstancedMesh();
    g->root()->addChild(node);
    if (const auto* srcMesh = bromesh::api::meshDataOf(bronze::Value{meshVal})) {
        node->setMesh(*srcMesh);
    }
    Value opts = ev::fromBits(optsBits);
    if (ev::isObject(opts)) applyInstancedOpts(node, opts);
    return wrapNode(node, g);
}

void* bro_scene_SceneGraph_createShape(void* self, const char* jsonOpts) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->createShape();
    if (jsonOpts && jsonOpts[0]) {
        try {
            auto j = nlohmann::json::parse(jsonOpts);
            if (j.contains("name") && j["name"].is_string()) node->setName(j["name"].get<std::string>());
            if (j.contains("shape") && j["shape"].is_string()) {
                const std::string s = j["shape"].get<std::string>();
                using S = scene::ShapeNode::Shape;
                if (s == "rect") node->setShape(S::Rect);
                else if (s == "roundrect") node->setShape(S::RoundRect);
                else if (s == "circle") node->setShape(S::Circle);
                else if (s == "ellipse") node->setShape(S::Ellipse);
                else if (s == "polygon") node->setShape(S::Polygon);
                else if (s == "line") node->setShape(S::Line);
            }
            auto num = [&](const char* k, float def) -> float {
                return (j.contains(k) && j[k].is_number()) ? j[k].get<float>() : def;
            };
            if (j.contains("width") || j.contains("height")) {
                node->setSize(num("width", 0.0f), num("height", 0.0f));
            }
            if (j.contains("radius") && j["radius"].is_number()) node->setRadius(num("radius", 0.0f));
            if (j.contains("cornerRadius") && j["cornerRadius"].is_number()) {
                node->setCornerRadius(num("cornerRadius", 0.0f));
            }
            if (j.contains("radiusX") || j.contains("radiusY")) {
                node->setRadii(num("radiusX", 0.0f), num("radiusY", 0.0f));
            }
            if (j.contains("fill") && j["fill"].is_string()) {
                float r = 1, g = 1, b = 1, a = 1;
                if (parseHexOrCssColor(j["fill"].get<std::string>(), r, g, b, a)) {
                    node->setFillColor(bromath::Color{r, g, b, a});
                }
            }
            if (j.contains("stroke") && j["stroke"].is_string()) {
                float r = 1, g = 1, b = 1, a = 1;
                if (parseHexOrCssColor(j["stroke"].get<std::string>(), r, g, b, a)) {
                    node->setStrokeColor(bromath::Color{r, g, b, a});
                }
            }
            if (j.contains("strokeWidth") && j["strokeWidth"].is_number()) {
                node->setStrokeWidth(num("strokeWidth", 0.0f));
                node->setHasStroke(true);
            }
            if (j.contains("anchorX") || j.contains("anchorY")) {
                node->setAnchor(num("anchorX", 0.5f), num("anchorY", 0.5f));
            }
            if (j.contains("points") && j["points"].is_array()) {
                std::vector<float> pts;
                pts.reserve(j["points"].size());
                for (const auto& p : j["points"]) pts.push_back(p.is_number() ? p.get<float>() : 0.0f);
                node->setPoints(pts);
            }
            if (j.contains("worldAnchor")) {
                const auto& wa = j["worldAnchor"];
                if (wa.is_array() && wa.size() >= 3) {
                    node->setWorldAnchor(bromath::Vec3{wa[0].get<float>(), wa[1].get<float>(), wa[2].get<float>()});
                }
            }
            if (j.contains("billboard") && j["billboard"].is_string()) {
                std::string m = j["billboard"].get<std::string>();
                if (m == "ylock" || m == "yLock") node->setBillboardMode(scene::SceneNode::BillboardMode::YLock);
                else node->setBillboardMode(scene::SceneNode::BillboardMode::Full);
            }
        } catch (const std::exception& e) {
            LOG_WARN("SceneGraph.createShape: failed to parse jsonOpts: %s", e.what());
        } catch (...) {
            LOG_WARN("SceneGraph.createShape: unknown exception while parsing jsonOpts");
        }
    }
    g->root()->addChild(node);
    return wrapNode(node, g);
}

void* bro_scene_SceneGraph_createSprite(void* self, const char* jsonOpts) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->createSprite();
    if (jsonOpts && jsonOpts[0]) {
        try {
            auto j = nlohmann::json::parse(jsonOpts);
            if (j.contains("sheet") && j["sheet"].is_object()) {
                const auto& s = j["sheet"];
                node->setSheetGrid(s.value("frameWidth", 0), s.value("frameHeight", 0), s.value("columns", 1), s.value("rows", 1));
            }
            if (j.contains("animations") && j["animations"].is_object()) {
                for (auto it = j["animations"].begin(); it != j["animations"].end(); ++it) {
                    const auto& specVal = it.value();
                    if (specVal.is_object()) {
                        scene::SpriteNode::AnimationSpec spec;
                        spec.fps = specVal.value("fps", 12.0f);
                        spec.loop = specVal.value("loop", true);
                        spec.next = specVal.value("next", "");
                        if (specVal.contains("frames") && specVal["frames"].is_array()) {
                            for (const auto& f : specVal["frames"]) if (f.is_number()) spec.frames.push_back(f.template get<int>());
                        }
                        node->addAnimation(it.key(), std::move(spec));
                    }
                }
            }
            // The explicit-frame form of `sheet`: { frames: [{x,y,w,h}, ...] }.
            if (j.contains("sheet") && j["sheet"].is_object() && j["sheet"].contains("frames") &&
                j["sheet"]["frames"].is_array()) {
                std::vector<scene::SpriteNode::Frame> frames;
                for (const auto& f : j["sheet"]["frames"]) {
                    if (!f.is_object()) continue;
                    scene::SpriteNode::Frame fr{};
                    fr.x = f.value("x", 0.0f);
                    fr.y = f.value("y", 0.0f);
                    fr.w = f.value("w", 0.0f);
                    fr.h = f.value("h", 0.0f);
                    frames.push_back(fr);
                }
                node->setSheetFrames(std::move(frames));
            }
            if (j.contains("name") && j["name"].is_string()) node->setName(j["name"].get<std::string>());
            if (j.contains("src") && j["src"].is_string()) {
                node->setImagePath(bro::util::resolveAssetPath(j["src"].get<std::string>()));
            }
            if (j.contains("width") || j.contains("height")) node->setSize(j.value("width", 0.0f), j.value("height", 0.0f));
            if (j.contains("opacity")) node->setOpacity(j.value("opacity", 1.0f));
            if (j.contains("anchorX") || j.contains("anchorY")) {
                node->setAnchor(j.value("anchorX", 0.5f), j.value("anchorY", 0.5f));
            }
            if (j.contains("play") && j["play"].is_string()) node->play(j["play"].get<std::string>());
        } catch (const std::exception& e) {
            LOG_WARN("SceneGraph.createSprite: failed to parse jsonOpts: %s", e.what());
        } catch (...) {
            LOG_WARN("SceneGraph.createSprite: unknown exception while parsing jsonOpts");
        }
    }
    g->root()->addChild(node);
    return wrapNode(node, g);
}

void* bro_scene_SceneGraph_createPhysicsNode(void* self, const char* jsonOpts) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->createPhysicsNode();
    if (!node) return nullptr;
    if (jsonOpts && jsonOpts[0]) {
        try {
            auto j = nlohmann::json::parse(jsonOpts);
            if (j.contains("name") && j["name"].is_string()) {
                node->setName(j["name"].get<std::string>());
            }
            if (j.contains("autoSync") && j["autoSync"].is_boolean()) {
                node->setAutoSync(j["autoSync"].get<bool>());
            }
            if (j.contains("pixelsPerUnit") && j["pixelsPerUnit"].is_number()) {
                node->setPixelsPerUnit(j["pixelsPerUnit"].get<float>());
            } else if (j.contains("pixelsPerMeter") && j["pixelsPerMeter"].is_number()) {
                node->setPixelsPerUnit(j["pixelsPerMeter"].get<float>());
            }
            if (j.contains("bodyId") && j["bodyId"].is_number_unsigned()) {
                node->setBody(JPH::BodyID(j["bodyId"].get<uint32_t>()));
            }
        } catch (const std::exception& e) {
            LOG_WARN("SceneGraph.createPhysicsNode: failed to parse jsonOpts: %s", e.what());
        } catch (...) {
            LOG_WARN("SceneGraph.createPhysicsNode: unknown exception while parsing jsonOpts");
        }
    }
    g->root()->addChild(node);
    return wrapNode(node, g);
}

void* bro_scene_SceneGraph_createParticles3D(void* self, const char* jsonOpts) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->createParticles3D();
    if (jsonOpts && jsonOpts[0]) {
        try {
            auto j = nlohmann::json::parse(jsonOpts);
            if (j.contains("name") && j["name"].is_string()) node->setName(j["name"].get<std::string>());
            if (j.contains("seed") && j["seed"].is_number()) node->setSeed(j["seed"].get<uint64_t>());
            if (j.contains("rate") && j["rate"].is_number()) node->setRate(j["rate"].get<float>());
            if (j.contains("lifetime")) {
                if (j["lifetime"].is_number()) {
                    float lt = j["lifetime"].get<float>();
                    node->setLifetime(lt, lt);
                } else if (j["lifetime"].is_object()) {
                    float lo = j["lifetime"].value("min", 0.5f);
                    float hi = j["lifetime"].value("max", 1.0f);
                    node->setLifetime(lo, hi);
                }
            }
            if (j.contains("size")) {
                if (j["size"].is_number()) {
                    float s = j["size"].get<float>();
                    node->setSize(s, s);
                } else if (j["size"].is_object()) {
                    float st = j["size"].value("start", 0.2f);
                    float en = j["size"].value("end", 0.2f);
                    node->setSize(st, en);
                }
            }
            if (j.contains("position")) {
                const auto& p = j["position"];
                if (p.is_array() && p.size() >= 3) {
                    node->setPosition(p[0].get<float>(), p[1].get<float>(), p[2].get<float>());
                }
            }
            if (j.contains("maxParticles") && j["maxParticles"].is_number()) node->setMaxParticles(j["maxParticles"].get<int>());
            if (j.contains("softness") && j["softness"].is_number()) node->setSoftness(j["softness"].get<float>());
            if (j.contains("duration") && j["duration"].is_number()) node->setDuration(j["duration"].get<float>(), j.value("loop", false));
            if (j.contains("blend") && j["blend"].is_string()) {
                node->setBlend(j["blend"] == "additive" ? scene::Particles3DNode::Blend::Additive : scene::Particles3DNode::Blend::Normal);
            }
            if (j.contains("space") && j["space"].is_string()) {
                node->setSpace(j["space"] == "local" ? scene::Particles3DNode::SimSpace::Local : scene::Particles3DNode::SimSpace::World);
            }
            if (j.contains("gravity") && j["gravity"].is_array() && j["gravity"].size() >= 3) {
                node->setGravity({j["gravity"][0].get<float>(), j["gravity"][1].get<float>(), j["gravity"][2].get<float>()});
            }
            if (j.contains("drag") && j["drag"].is_number()) node->setDrag(j["drag"].get<float>());
            if (j.contains("velocity") && j["velocity"].is_object()) {
                const auto& vel = j["velocity"];
                float sp = vel.value("speed", 1.0f);
                float spSpr = vel.value("speedSpread", 0.0f);
                node->setSpeed(sp, spSpr);
                float spr = vel.value("spread", 0.0f);
                bromath::Vec3 dir{0, 1, 0};
                if (vel.contains("direction") && vel["direction"].is_array() && vel["direction"].size() >= 3) {
                    dir = {vel["direction"][0].get<float>(), vel["direction"][1].get<float>(), vel["direction"][2].get<float>()};
                }
                node->setDirection(dir, spr);
            }
            if (j.contains("rotation") && j["rotation"].is_object()) {
                const auto& r = j["rotation"];
                node->setRotation(r.value("start", 0.0f), r.value("spinSpeed", 0.0f), r.value("spinSpread", 0.0f));
            }
            if (j.contains("shape") && j["shape"].is_object()) {
                const auto& sh = j["shape"];
                std::string type = sh.value("type", "point");
                if (type == "sphere") node->setShape(scene::Particles3DNode::EmitterShape::Sphere);
                else if (type == "hemisphere") node->setShape(scene::Particles3DNode::EmitterShape::Hemisphere);
                else if (type == "box") node->setShape(scene::Particles3DNode::EmitterShape::Box);
                else if (type == "cone") node->setShape(scene::Particles3DNode::EmitterShape::Cone);
                else node->setShape(scene::Particles3DNode::EmitterShape::Point);
                if (sh.contains("radius") && sh["radius"].is_number()) node->setShapeRadius(sh["radius"].get<float>());
            }
            if (j.contains("color") && j["color"].is_object()) {
                const auto& parseC = bro::bronze_host::particleColorFromJson;
                const auto& c = j["color"];
                bromath::Color st = c.contains("start") ? parseC(c["start"]) : bromath::Color{1,1,1,1};
                bromath::Color en = c.contains("end") ? parseC(c["end"]) : st;
                node->setColors(st, en);
            }
            if (j.contains("burst") && j["burst"].is_number()) {
                node->burst(j["burst"].get<int>());
            }
            if (j.value("autoplay", true)) node->play();
        } catch (const std::exception& e) {
            LOG_WARN("SceneGraph.createParticles3D: failed to parse jsonOpts: %s", e.what());
        } catch (...) {
            LOG_WARN("SceneGraph.createParticles3D: unknown exception while parsing jsonOpts");
        }
    }
    g->root()->addChild(node);
    return wrapNode(node, g);
}

void* bro_scene_SceneGraph_createGaussianSplat(void* self, const char* jsonOpts) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->createGaussianSplat();
    g->root()->addChild(node);
    return wrapNode(node, g);
}

}  // extern "C"
