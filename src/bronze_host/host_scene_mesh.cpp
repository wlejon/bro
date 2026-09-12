#if BRO_WITH_3D

#include "bronze_host/host_scene_internal.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/mesh_node.h"
#include "scene/skinned_mesh_node.h"
#include "scene/instanced_mesh_node.h"
#include "scene/decal_node.h"

#include <bromesh/primitives/primitives.h>
#include <bromesh/manipulation/normals.h>
#include "bronze_host/host_mesh_internal.h"
#include "bronze_host/host_rigging_internal.h"

#include <cmath>
#include <vector>

namespace bro::bronze_host {

namespace {

double numAtProp(Value obj, const char* key, double defVal) {
    if (!ev::isObject(obj)) return defVal;
    Value v = ev::getProperty(obj, key);
    return ev::isNumber(v) ? ev::toDouble(v) : defVal;
}

std::string strAtProp(Value obj, const char* key, const std::string& defVal) {
    if (!ev::isObject(obj)) return defVal;
    Value v = ev::getProperty(obj, key);
    return ev::isString(v) ? ev::toUtf8(v) : defVal;
}


bool readUint32Vector(Value v, std::vector<uint32_t>& out) {
    if (auto info = ev::typedArrayInfo(v)) {
        size_t count = info.byteLength / sizeof(uint32_t);
        const uint32_t* p = reinterpret_cast<const uint32_t*>(info.data);
        out.assign(p, p + count);
        return true;
    }
    if (ev::isObject(v)) {
        Value lenV = ev::getProperty(v, "length");
        if (ev::isNumber(lenV)) {
            size_t count = static_cast<size_t>(ev::toDouble(lenV));
            out.resize(count);
            for (size_t i = 0; i < count; ++i) {
                out[i] = static_cast<uint32_t>(ev::toDouble(ev::getElement(v, i)));
            }
            return true;
        }
    }
    return false;
}

void applyMeshNodeOptions(scene::MeshNode* node, Value opts) {
    Value nameVal = ev::getProperty(opts, "name");
    if (ev::isString(nameVal)) node->setName(ev::toUtf8(nameVal));

    Value visibleVal = ev::getProperty(opts, "visible");
    if (!ev::isUndefined(visibleVal)) node->setVisible(ev::toBool(visibleVal));

    double x = numAtProp(opts, "x", 0.0);
    double y = numAtProp(opts, "y", 0.0);
    double z = numAtProp(opts, "z", 0.0);
    node->setPosition(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));

    Value scaleVal = ev::getProperty(opts, "scale");
    if (!ev::isUndefined(scaleVal)) {
        if (ev::isNumber(scaleVal)) {
            float s = static_cast<float>(ev::toDouble(scaleVal));
            node->setScale(s, s, s);
        } else if (ev::isObject(scaleVal)) {
            bromath::Vec3 s{1, 1, 1};
            Value e0 = ev::getElement(scaleVal, 0);
            Value e1 = ev::getElement(scaleVal, 1);
            Value e2 = ev::getElement(scaleVal, 2);
            if (ev::isNumber(e0)) s.x = static_cast<float>(ev::toDouble(e0));
            if (ev::isNumber(e1)) s.y = static_cast<float>(ev::toDouble(e1));
            if (ev::isNumber(e2)) s.z = static_cast<float>(ev::toDouble(e2));
            node->setScale(s.x, s.y, s.z);
        }
    }

    Value rxVal = ev::getProperty(opts, "rx");
    Value ryVal = ev::getProperty(opts, "ry");
    Value rzVal = ev::getProperty(opts, "rz");
    if (!ev::isUndefined(rxVal) || !ev::isUndefined(ryVal) || !ev::isUndefined(rzVal)) {
        float rx = ev::isNumber(rxVal) ? static_cast<float>(ev::toDouble(rxVal)) : 0.0f;
        float ry = ev::isNumber(ryVal) ? static_cast<float>(ev::toDouble(ryVal)) : 0.0f;
        float rz = ev::isNumber(rzVal) ? static_cast<float>(ev::toDouble(rzVal)) : 0.0f;
        constexpr float toRad = 3.141592653589793f / 180.0f;
        node->setRotationEuler(rx * toRad, ry * toRad, rz * toRad);
    }

    Value colorVal = ev::getProperty(opts, "color");
    float cr = 1, cg = 1, cb = 1, ca = 1;
    if (parseColorValue(colorVal, cr, cg, cb, ca)) {
        node->setColor(cr, cg, cb, ca);
    }

    double emissive = numAtProp(opts, "emissive", 0.0);
    node->setEmissive(static_cast<float>(emissive));

    Value emColVal = ev::getProperty(opts, "emissiveColor");
    float er = 1, eg = 1, eb = 1, ea = 1;
    if (parseColorValue(emColVal, er, eg, eb, ea)) {
        node->setEmissiveColor(er, eg, eb);
    } else if (emissive > 0.0) {
        const float* c = node->color();
        node->setEmissiveColor(c[0], c[1], c[2]);
    }

    Value matVal = ev::getProperty(opts, "material");
    auto applyMat = [&](Value obj) {
        Value mv = ev::getProperty(obj, "metallic");
        if (!ev::isUndefined(mv)) node->setMetallic(static_cast<float>(ev::toDouble(mv)));
        Value rv = ev::getProperty(obj, "roughness");
        if (!ev::isUndefined(rv)) node->setRoughness(static_cast<float>(ev::toDouble(rv)));
    };
    if (ev::isObject(matVal)) applyMat(matVal);
    applyMat(opts);

    Value unlitVal = ev::getProperty(opts, "unlit");
    if (!ev::isUndefined(unlitVal)) node->setUnlit(ev::toBool(unlitVal));

    Value tsVal = ev::getProperty(opts, "twoSided");
    if (!ev::isUndefined(tsVal)) node->setTwoSided(ev::toBool(tsVal));

    Value ssVal = ev::getProperty(opts, "subsurface");
    if (ev::isNumber(ssVal)) node->setSubsurface(static_cast<float>(ev::toDouble(ssVal)));

    Value acVal = ev::getProperty(opts, "alphaCutoff");
    if (ev::isNumber(acVal)) node->setAlphaCutoff(static_cast<float>(ev::toDouble(acVal)));

    Value vctVal = ev::getProperty(opts, "vertexColorTint");
    if (!ev::isUndefined(vctVal)) node->setVertexColorTint(ev::toBool(vctVal));

    Value dmVal = ev::getProperty(opts, "drawMode");
    if (ev::isString(dmVal)) {
        std::string s = ev::toUtf8(dmVal);
        if (s == "lines" || s == "line")
            node->setDrawMode(scene::MeshNode::DrawMode::Lines);
        else
            node->setDrawMode(scene::MeshNode::DrawMode::Triangles);
    }

    Value lwVal = ev::getProperty(opts, "lineWidth");
    if (ev::isNumber(lwVal)) node->setLineWidth(static_cast<float>(ev::toDouble(lwVal)));

    Value wmVal = ev::getProperty(opts, "wind");
    if (!ev::isUndefined(wmVal)) {
        if (ev::isBool(wmVal)) {
            node->setWindMask(ev::toBool(wmVal) ? 1.0f : 0.0f);
        } else if (ev::isNumber(wmVal)) {
            node->setWindMask(static_cast<float>(ev::toDouble(wmVal)));
        }
    }

    Value csVal = ev::getProperty(opts, "castsShadow");
    if (!ev::isUndefined(csVal)) node->setCastsShadow(ev::toBool(csVal));
    Value rsVal = ev::getProperty(opts, "receivesShadow");
    if (!ev::isUndefined(rsVal)) node->setReceivesShadow(ev::toBool(rsVal));

    Value dbVal = ev::getProperty(opts, "depthBias");
    if (!ev::isUndefined(dbVal)) {
        if (ev::isNumber(dbVal)) {
            node->setDepthBias(0.0f, static_cast<float>(ev::toDouble(dbVal)));
        } else if (ev::isObject(dbVal)) {
            float f = static_cast<float>(ev::toDouble(ev::getElement(dbVal, 0)));
            float u = static_cast<float>(ev::toDouble(ev::getElement(dbVal, 1)));
            node->setDepthBias(f, u);
        }
    }

    bromesh::MeshData meshData;
    bool hasRawData = false;

    Value meshVal = ev::getProperty(opts, "mesh");
    if (ev::isObject(meshVal)) {
        if (auto* m = hostMeshDataOf(meshVal)) {
            meshData = *m;
            hasRawData = true;
        } else {
            Value pVal = ev::getProperty(meshVal, "positions");
            Value iVal = ev::getProperty(meshVal, "indices");
            if (!ev::isUndefined(pVal) && !ev::isUndefined(iVal)) {
                if (readFloatVector(pVal, meshData.positions) && readUint32Vector(iVal, meshData.indices)) {
                    readFloatVector(ev::getProperty(meshVal, "normals"), meshData.normals);
                    readFloatVector(ev::getProperty(meshVal, "colors"), meshData.colors);
                    readFloatVector(ev::getProperty(meshVal, "uvs"), meshData.uvs);
                    readFloatVector(ev::getProperty(meshVal, "tangents"), meshData.tangents);
                    hasRawData = true;
                }
            }
        }
    }

    if (!hasRawData) {
        Value posVal = ev::getProperty(opts, "positions");
        Value idxVal = ev::getProperty(opts, "indices");
        if (!ev::isUndefined(posVal) && !ev::isUndefined(idxVal)) {
            if (readFloatVector(posVal, meshData.positions) && readUint32Vector(idxVal, meshData.indices)) {
                readFloatVector(ev::getProperty(opts, "normals"), meshData.normals);
                readFloatVector(ev::getProperty(opts, "colors"), meshData.colors);
                readFloatVector(ev::getProperty(opts, "uvs"), meshData.uvs);
                readFloatVector(ev::getProperty(opts, "tangents"), meshData.tangents);
                hasRawData = true;
            }
        }
    }

    if (!hasRawData) {
        std::string meshType = strAtProp(opts, "mesh", "box");
        if (meshType == "box") {
            float hw = static_cast<float>(numAtProp(opts, "halfW", 0.5));
            float hh = static_cast<float>(numAtProp(opts, "halfH", 0.5));
            float hd = static_cast<float>(numAtProp(opts, "halfD", 0.5));
            meshData = bromesh::box(hw, hh, hd);
        } else if (meshType == "sphere") {
            float radius = static_cast<float>(numAtProp(opts, "radius", 0.5));
            int segments = static_cast<int>(numAtProp(opts, "segments", 16));
            int rings = static_cast<int>(numAtProp(opts, "rings", 12));
            meshData = bromesh::sphere(radius, segments, rings);
        } else if (meshType == "cylinder") {
            float radius = static_cast<float>(numAtProp(opts, "radius", 0.5));
            float height = static_cast<float>(numAtProp(opts, "height", 1.0));
            int segments = static_cast<int>(numAtProp(opts, "segments", 16));
            meshData = bromesh::cylinder(radius, height, segments);
        } else if (meshType == "capsule") {
            float radius = static_cast<float>(numAtProp(opts, "radius", 0.5));
            float height = static_cast<float>(numAtProp(opts, "height", 1.0));
            int segments = static_cast<int>(numAtProp(opts, "segments", 16));
            int rings = static_cast<int>(numAtProp(opts, "rings", 8));
            meshData = bromesh::capsule(radius, height, segments, rings);
        } else if (meshType == "plane") {
            float hw = static_cast<float>(numAtProp(opts, "halfW", 5.0));
            float hd = static_cast<float>(numAtProp(opts, "halfD", 5.0));
            int sx = static_cast<int>(numAtProp(opts, "subdivX", 1));
            int sz = static_cast<int>(numAtProp(opts, "subdivZ", 1));
            meshData = bromesh::plane(hw, hd, sx, sz);
        } else if (meshType == "torus") {
            float major = static_cast<float>(numAtProp(opts, "majorRadius", 1.0));
            float minor = static_cast<float>(numAtProp(opts, "minorRadius", 0.3));
            int majSeg = static_cast<int>(numAtProp(opts, "majorSegments", 24));
            int minSeg = static_cast<int>(numAtProp(opts, "minorSegments", 12));
            meshData = bromesh::torus(major, minor, majSeg, minSeg);
        }
    }

    if (meshData.normals.empty() && !meshData.positions.empty()) {
        bromesh::computeNormals(meshData);
    }
    node->setMesh(std::move(meshData));

    auto applyTex = [&](const char* key, void (scene::MeshNode::*setter)(int, int, const uint8_t*)) {
        Value tex = ev::getProperty(opts, key);
        if (ev::isObject(tex)) {
            int w = static_cast<int>(numAtProp(tex, "width", 0));
            int h = static_cast<int>(numAtProp(tex, "height", 0));
            Value dataVal = ev::getProperty(tex, "data");
            ev::TypedArrayInfo info = ev::typedArrayInfo(dataVal);
            if (info && info.data && w > 0 && h > 0 && info.byteLength >= static_cast<size_t>(w) * h * 4) {
                (node->*setter)(w, h, info.data);
            }
        }
    };

    applyTex("texture",                  &scene::MeshNode::setBaseColorTexture);
    applyTex("normalTexture",            &scene::MeshNode::setNormalTexture);
    applyTex("metallicRoughnessTexture", &scene::MeshNode::setMetallicRoughnessTexture);
    applyTex("occlusionTexture",         &scene::MeshNode::setOcclusionTexture);
    applyTex("emissiveTexture",          &scene::MeshNode::setEmissiveTexture);
}

void applyInstancedMeshNodeOptions(scene::InstancedMeshNode* node, Value opts) {
    Value nameVal = ev::getProperty(opts, "name");
    if (ev::isString(nameVal)) node->setName(ev::toUtf8(nameVal));

    Value visibleVal = ev::getProperty(opts, "visible");
    if (!ev::isUndefined(visibleVal)) node->setVisible(ev::toBool(visibleVal));

    double x = numAtProp(opts, "x", 0.0);
    double y = numAtProp(opts, "y", 0.0);
    double z = numAtProp(opts, "z", 0.0);
    node->setPosition(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));

    Value scaleVal = ev::getProperty(opts, "scale");
    if (!ev::isUndefined(scaleVal)) {
        if (ev::isNumber(scaleVal)) {
            float s = static_cast<float>(ev::toDouble(scaleVal));
            node->setScale(s, s, s);
        } else if (ev::isObject(scaleVal)) {
            bromath::Vec3 s{1, 1, 1};
            Value e0 = ev::getElement(scaleVal, 0);
            Value e1 = ev::getElement(scaleVal, 1);
            Value e2 = ev::getElement(scaleVal, 2);
            if (ev::isNumber(e0)) s.x = static_cast<float>(ev::toDouble(e0));
            if (ev::isNumber(e1)) s.y = static_cast<float>(ev::toDouble(e1));
            if (ev::isNumber(e2)) s.z = static_cast<float>(ev::toDouble(e2));
            node->setScale(s.x, s.y, s.z);
        }
    }

    Value colorVal = ev::getProperty(opts, "color");
    float cr = 1, cg = 1, cb = 1, ca = 1;
    if (parseColorValue(colorVal, cr, cg, cb, ca)) {
        node->setColor(cr, cg, cb, ca);
    }

    bromesh::MeshData meshData;
    bool hasRawData = false;

    Value meshVal = ev::getProperty(opts, "mesh");
    if (ev::isObject(meshVal)) {
        Value pVal = ev::getProperty(meshVal, "positions");
        Value iVal = ev::getProperty(meshVal, "indices");
        if (!ev::isUndefined(pVal) && !ev::isUndefined(iVal)) {
            if (readFloatVector(pVal, meshData.positions) && readUint32Vector(iVal, meshData.indices)) {
                readFloatVector(ev::getProperty(meshVal, "normals"), meshData.normals);
                readFloatVector(ev::getProperty(meshVal, "colors"), meshData.colors);
                readFloatVector(ev::getProperty(meshVal, "uvs"), meshData.uvs);
                readFloatVector(ev::getProperty(meshVal, "tangents"), meshData.tangents);
                hasRawData = true;
            }
        }
    }

    if (!hasRawData) {
        Value posVal = ev::getProperty(opts, "positions");
        Value idxVal = ev::getProperty(opts, "indices");
        if (!ev::isUndefined(posVal) && !ev::isUndefined(idxVal)) {
            if (readFloatVector(posVal, meshData.positions) && readUint32Vector(idxVal, meshData.indices)) {
                readFloatVector(ev::getProperty(opts, "normals"), meshData.normals);
                readFloatVector(ev::getProperty(opts, "colors"), meshData.colors);
                readFloatVector(ev::getProperty(opts, "uvs"), meshData.uvs);
                readFloatVector(ev::getProperty(opts, "tangents"), meshData.tangents);
                hasRawData = true;
            }
        }
    }

    if (!hasRawData) {
        std::string meshType = strAtProp(opts, "mesh", "box");
        if (meshType == "box") {
            float hw = static_cast<float>(numAtProp(opts, "halfW", 0.5));
            float hh = static_cast<float>(numAtProp(opts, "halfH", 0.5));
            float hd = static_cast<float>(numAtProp(opts, "halfD", 0.5));
            meshData = bromesh::box(hw, hh, hd);
        } else if (meshType == "sphere") {
            float radius = static_cast<float>(numAtProp(opts, "radius", 0.5));
            int segments = static_cast<int>(numAtProp(opts, "segments", 16));
            int rings = static_cast<int>(numAtProp(opts, "rings", 12));
            meshData = bromesh::sphere(radius, segments, rings);
        } else if (meshType == "plane") {
            float hw = static_cast<float>(numAtProp(opts, "halfW", 5.0));
            float hd = static_cast<float>(numAtProp(opts, "halfD", 5.0));
            int sx = static_cast<int>(numAtProp(opts, "subdivX", 1));
            int sz = static_cast<int>(numAtProp(opts, "subdivZ", 1));
            meshData = bromesh::plane(hw, hd, sx, sz);
        }
    }

    if (meshData.normals.empty() && !meshData.positions.empty()) {
        bromesh::computeNormals(meshData);
    }
    node->setMesh(std::move(meshData));
}

}  // namespace

void installSceneGraphMesh(ObjectBuilder& b) {
    b.def("createMesh", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::undefined();
        auto* node = g->createMesh();
        g->root()->addChild(node);
        if (!a.empty() && ev::isObject(a[0])) {
            applyMeshNodeOptions(node, a[0]);
        }
        return wrapSceneNode(node, g);
    });

    b.def("createSkinnedMesh", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::undefined();
        if (a.empty() || !ev::isObject(a[0]))
            return ev::throwTypeError("createSkinnedMesh requires an options object with mesh + skin");

        auto* node = g->createSkinnedMesh();
        g->root()->addChild(node);
        applyMeshNodeOptions(node, a[0]);

        Value skinVal = ev::getProperty(a[0], "skin");
        auto* sd = hostSkinDataOf(skinVal);
        if (!sd) {
            g->destroyNode(node);
            return ev::throwTypeError("createSkinnedMesh: opts.skin must be a SkinData");
        }
        if (!node->setSkin(*sd)) {
            g->destroyNode(node);
            return ev::throwTypeError("createSkinnedMesh: skin rejected (bone count 0 or > 256, or weight/index streams malformed)");
        }
        if (!node->skinReady()) {
            g->destroyNode(node);
            return ev::throwTypeError("createSkinnedMesh: skin vertex count does not match the mesh");
        }

        std::vector<float> palette;
        if (readFloatVector(ev::getProperty(a[0], "skinningMatrices"), palette) && palette.size() >= 16) {
            node->setSkinningMatrices(palette.data(), palette.size() / 16);
        }

        return wrapSceneNode(node, g);
    });

    b.def("createInstancedMesh", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::undefined();
        auto* node = g->createInstancedMesh();
        g->root()->addChild(node);
        if (!a.empty() && ev::isObject(a[0])) {
            applyInstancedMeshNodeOptions(node, a[0]);
        }
        return wrapSceneNode(node, g);
    });
}

void installSceneNodeMesh(ObjectBuilder& b) {
    b.accessor("metallic",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Mesh)
                return ev::fromDouble(static_cast<scene::MeshNode*>(n)->metallic());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Mesh && !a.empty())
                static_cast<scene::MeshNode*>(n)->setMetallic(static_cast<float>(ev::toDouble(a[0])));
            return ev::undefined();
        });

    b.accessor("roughness",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Mesh)
                return ev::fromDouble(static_cast<scene::MeshNode*>(n)->roughness());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Mesh && !a.empty())
                static_cast<scene::MeshNode*>(n)->setRoughness(static_cast<float>(ev::toDouble(a[0])));
            return ev::undefined();
        });

    b.accessor("emissive",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Mesh)
                return ev::fromDouble(static_cast<scene::MeshNode*>(n)->emissive());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Mesh && !a.empty())
                static_cast<scene::MeshNode*>(n)->setEmissive(static_cast<float>(ev::toDouble(a[0])));
            return ev::undefined();
        });

    b.accessor("emissiveColor",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (!n || n->type() != scene::SceneNode::Type::Mesh) return ev::undefined();
            const float* c = static_cast<scene::MeshNode*>(n)->emissiveColor();
            return hostArrayOf(3, [c](size_t i) { return ev::fromDouble(c[i]); });
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (!n || n->type() != scene::SceneNode::Type::Mesh || a.empty()) return ev::undefined();
            float r = 1, g = 1, b_ = 1, a_ = 1;
            if (parseColorValue(a[0], r, g, b_, a_)) {
                static_cast<scene::MeshNode*>(n)->setEmissiveColor(r, g, b_);
            }
            return ev::undefined();
        });

    b.accessor("color",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (!n) return ev::undefined();
            if (n->type() == scene::SceneNode::Type::Light) {
                const auto& c = static_cast<scene::LightNode*>(n)->color();
                return hostArrayOf(3, [&c](size_t i) {
                    if (i == 0) return ev::fromDouble(c.x);
                    if (i == 1) return ev::fromDouble(c.y);
                    return ev::fromDouble(c.z);
                });
            }
            if (n->type() == scene::SceneNode::Type::Mesh) {
                const float* c = static_cast<scene::MeshNode*>(n)->color();
                return hostArrayOf(4, [c](size_t i) { return ev::fromDouble(c[i]); });
            }
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (!n || a.empty()) return ev::undefined();
            float r = 1, g = 1, b_ = 1, a_ = -1;
            if (!parseColorValue(a[0], r, g, b_, a_)) return ev::undefined();
            if (n->type() == scene::SceneNode::Type::Light) {
                static_cast<scene::LightNode*>(n)->setColor(r, g, b_);
            } else if (n->type() == scene::SceneNode::Type::Mesh) {
                auto* M = static_cast<scene::MeshNode*>(n);
                M->setColor(r, g, b_, a_ >= 0 ? a_ : M->color()[3]);
            }
            return ev::undefined();
        });

    b.accessor("castsShadow",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (!n) return ev::fromBool(false);
            if (n->type() == scene::SceneNode::Type::Light)
                return ev::fromBool(static_cast<scene::LightNode*>(n)->castsShadow());
            if (n->type() == scene::SceneNode::Type::Mesh)
                return ev::fromBool(static_cast<scene::MeshNode*>(n)->castsShadow());
            return ev::fromBool(false);
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (!n || a.empty()) return ev::undefined();
            bool val = ev::toBool(a[0]);
            if (n->type() == scene::SceneNode::Type::Light)
                static_cast<scene::LightNode*>(n)->setCastsShadow(val);
            else if (n->type() == scene::SceneNode::Type::Mesh)
                static_cast<scene::MeshNode*>(n)->setCastsShadow(val);
            return ev::undefined();
        });

    b.accessor("receivesShadow",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Mesh)
                return ev::fromBool(static_cast<scene::MeshNode*>(n)->receivesShadow());
            return ev::fromBool(false);
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Mesh && !a.empty())
                static_cast<scene::MeshNode*>(n)->setReceivesShadow(ev::toBool(a[0]));
            return ev::undefined();
        });

    b.accessor("nearClipDist",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Mesh)
                return ev::fromDouble(static_cast<scene::MeshNode*>(n)->nearClipDist());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Mesh && !a.empty())
                static_cast<scene::MeshNode*>(n)->setNearClipDist(static_cast<float>(ev::toDouble(a[0])));
            return ev::undefined();
        });

    b.accessor("cullMargin",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n) {
                if (n->type() == scene::SceneNode::Type::Mesh)
                    return ev::fromDouble(static_cast<scene::MeshNode*>(n)->cullMargin());
                if (n->type() == scene::SceneNode::Type::InstancedMesh)
                    return ev::fromDouble(static_cast<scene::InstancedMeshNode*>(n)->cullMargin());
            }
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && !a.empty()) {
                float val = static_cast<float>(ev::toDouble(a[0]));
                if (n->type() == scene::SceneNode::Type::Mesh)
                    static_cast<scene::MeshNode*>(n)->setCullMargin(val);
                else if (n->type() == scene::SceneNode::Type::InstancedMesh)
                    static_cast<scene::InstancedMeshNode*>(n)->setCullMargin(val);
            }
            return ev::undefined();
        });

    b.accessor("hasShader", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (!n) return ev::fromBool(false);
        if (n->type() == scene::SceneNode::Type::Mesh)
            return ev::fromBool(static_cast<scene::MeshNode*>(n)->hasCustomShader());
        if (n->type() == scene::SceneNode::Type::InstancedMesh)
            return ev::fromBool(static_cast<scene::InstancedMeshNode*>(n)->hasCustomShader());
        return ev::fromBool(false);
    }, nullptr);

    b.accessor("lodLevel", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (n && n->type() == scene::SceneNode::Type::Mesh) {
            auto* m = static_cast<scene::MeshNode*>(n);
            if (m->hasLodChain()) return ev::fromDouble(m->selectedLod());
        }
        return ev::undefined();
    }, nullptr);

    b.accessor("lodCount", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (n && n->type() == scene::SceneNode::Type::Mesh)
            return ev::fromDouble(static_cast<scene::MeshNode*>(n)->lodCount());
        return ev::undefined();
    }, nullptr);

    b.accessor("instanceCount", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (n && n->type() == scene::SceneNode::Type::InstancedMesh)
            return ev::fromDouble(static_cast<scene::InstancedMeshNode*>(n)->instanceCount());
        return ev::fromDouble(0.0);
    }, nullptr);

    b.accessor("atlasCols", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (n && n->type() == scene::SceneNode::Type::InstancedMesh)
            return ev::fromDouble(static_cast<scene::InstancedMeshNode*>(n)->atlasCols());
        return ev::fromDouble(0.0);
    }, nullptr);

    b.accessor("atlasRows", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (n && n->type() == scene::SceneNode::Type::InstancedMesh)
            return ev::fromDouble(static_cast<scene::InstancedMeshNode*>(n)->atlasRows());
        return ev::fromDouble(0.0);
    }, nullptr);

    b.def("setBaseColorTexture", 1, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n || (n->type() != scene::SceneNode::Type::Mesh && n->type() != scene::SceneNode::Type::Decal))
            return ev::throwTypeError("setBaseColorTexture: not a MeshNode or DecalNode");

        if (n->type() == scene::SceneNode::Type::Decal) {
            auto* decal = static_cast<scene::DecalNode*>(n);
            if (a.empty() || ev::isNull(a[0]) || ev::isUndefined(a[0])) {
                decal->clearAlbedoTexture();
                return ev::undefined();
            }
            if (!ev::isObject(a[0])) return ev::throwTypeError("setBaseColorTexture: expected { width, height, data } or null");
            int w = static_cast<int>(numAtProp(a[0], "width", 0));
            int h = static_cast<int>(numAtProp(a[0], "height", 0));
            Value dataVal = ev::getProperty(a[0], "data");
            ev::TypedArrayInfo info = ev::typedArrayInfo(dataVal);
            if (info && info.data && w > 0 && h > 0 && info.byteLength >= static_cast<size_t>(w) * h * 4) {
                decal->setAlbedoTexture(w, h, info.data);
            }
            return ev::undefined();
        }

        auto* meshNode = static_cast<scene::MeshNode*>(n);
        if (a.empty() || ev::isNull(a[0]) || ev::isUndefined(a[0])) {
            meshNode->clearBaseColorTexture();
            return ev::undefined();
        }
        if (auto* h = sceneTextureCellOf(a[0])) {
            auto wk = h->src;
            meshNode->setExternalBaseColorTexture([wk]() -> unsigned {
                auto s = wk.lock();
                return (s && s->graph) ? s->graph->outputColorTexture() : 0u;
            });
            return ev::undefined();
        }
        if (!ev::isObject(a[0]))
            return ev::throwTypeError("setBaseColorTexture: expected { width, height, data }, SceneTexture, or null");
        int w = static_cast<int>(numAtProp(a[0], "width", 0));
        int h = static_cast<int>(numAtProp(a[0], "height", 0));
        Value dataVal = ev::getProperty(a[0], "data");
        ev::TypedArrayInfo info = ev::typedArrayInfo(dataVal);
        if (info && info.data && w > 0 && h > 0 && info.byteLength >= static_cast<size_t>(w) * h * 4) {
            meshNode->setBaseColorTexture(w, h, info.data);
        }
        return ev::undefined();
    });

    b.def("setInstances", 1, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n || n->type() != scene::SceneNode::Type::InstancedMesh)
            return ev::throwTypeError("setInstances: node is not an InstancedMeshNode");
        if (a.empty()) return self_;
        auto* inst = static_cast<scene::InstancedMeshNode*>(n);
        ev::TypedArrayInfo info = ev::typedArrayInfo(a[0]);
        if (info && info.data) {
            size_t floatCount = info.byteLength / sizeof(float);
            inst->setInstances(reinterpret_cast<const float*>(info.data), floatCount / 16);
        } else if (ev::isObject(a[0])) {
            std::vector<float> floats;
            readFloatVector(a[0], floats);
            inst->setInstances(floats.data(), floats.size() / 16);
        }
        return self_;
    });

    b.def("updateMesh", 2, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n || n->type() != scene::SceneNode::Type::Mesh)
            return ev::throwTypeError("updateMesh: node is not a MeshNode");
        if (a.empty()) return ev::throwTypeError("updateMesh: missing argument");
        auto* meshNode = static_cast<scene::MeshNode*>(n);
        bromesh::MeshData meshData;
        if (ev::isObject(a[0])) {
            readFloatVector(ev::getProperty(a[0], "positions"), meshData.positions);
            readUint32Vector(ev::getProperty(a[0], "indices"), meshData.indices);
            readFloatVector(ev::getProperty(a[0], "normals"), meshData.normals);
            readFloatVector(ev::getProperty(a[0], "colors"), meshData.colors);
            readFloatVector(ev::getProperty(a[0], "uvs"), meshData.uvs);
            readFloatVector(ev::getProperty(a[0], "tangents"), meshData.tangents);
            if (meshData.normals.empty()) bromesh::computeNormals(meshData);
            meshNode->setMesh(std::move(meshData));
        }
        return self_;
    });

    b.accessor("boneCount", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (n && n->type() == scene::SceneNode::Type::Mesh) {
            auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
            if (sm) return ev::fromDouble(sm->boneCount());
        }
        return ev::fromDouble(0.0);
    }, nullptr);

    b.accessor("skinReady", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (n && n->type() == scene::SceneNode::Type::Mesh) {
            auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
            if (sm) return ev::fromBool(sm->skinReady());
        }
        return ev::fromBool(false);
    }, nullptr);

    b.def("setSkinningMatrices", 1, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n || n->type() != scene::SceneNode::Type::Mesh)
            return ev::throwTypeError("setSkinningMatrices: not a mesh node");
        auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
        if (!sm)
            return ev::throwTypeError("setSkinningMatrices: node is not a skinned mesh (use createSkinnedMesh)");
        if (a.empty())
            return ev::throwTypeError("setSkinningMatrices: missing matrix array");
        std::vector<float> palette;
        if (!readFloatVector(a[0], palette))
            return ev::throwTypeError("setSkinningMatrices: expected a Float32Array");
        int nMat = sm->setSkinningMatrices(palette.data(), palette.size() / 16);
        return ev::fromDouble(nMat);
    });

    b.def("setLodMeshes", 1, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n || n->type() != scene::SceneNode::Type::Mesh)
            return ev::throwTypeError("setLodMeshes: not a MeshNode");
        auto* meshNode = static_cast<scene::MeshNode*>(n);
        if (meshNode->asSkinnedMesh())
            return ev::throwTypeError("setLodMeshes: not supported on skinned meshes");
        if (a.empty() || !ev::isObject(a[0]))
            return ev::throwTypeError("setLodMeshes: argument must be an array of {mesh, maxDist}");

        Value lenVal = ev::getProperty(a[0], "length");
        if (!ev::isNumber(lenVal))
            return ev::throwTypeError("setLodMeshes: argument must be an array of {mesh, maxDist}");
        uint32_t len = static_cast<uint32_t>(ev::toDouble(lenVal));

        std::vector<scene::MeshNode::LodLevel> levels;
        levels.reserve(len);
        for (uint32_t i = 0; i < len; ++i) {
            Value entry = ev::getElement(a[0], i);
            if (!ev::isObject(entry))
                return ev::throwTypeError("setLodMeshes: entry is not an object");
            Value meshVal = ev::getProperty(entry, "mesh");
            bromesh::MeshData* md = hostMeshDataOf(meshVal);
            if (!md)
                return ev::throwTypeError("setLodMeshes: entry has no Mesh in `mesh`");
            scene::MeshNode::LodLevel lv;
            lv.mesh = *md;
            lv.maxDist = static_cast<float>(numAtProp(entry, "maxDist", 1e30));
            levels.push_back(std::move(lv));
        }

        meshNode->setLodMeshes(std::move(levels));
        return self_;
    });
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
