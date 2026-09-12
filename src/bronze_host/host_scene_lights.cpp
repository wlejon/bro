#if BRO_WITH_3D

#include "bronze_host/host_scene_internal.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/light_node.h"
#include "scene/camera_node.h"
#include "scene/mesh_node.h"

#include <cmath>
#include <span>
#include <string>

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

}  // namespace

void installSceneGraphLights(ObjectBuilder& b) {
    b.def("createLight", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::undefined();
        auto* node = g->createLight();
        g->root()->addChild(node);
        if (!a.empty() && ev::isObject(a[0])) {
            Value opts = a[0];
            Value nameVal = ev::getProperty(opts, "name");
            if (ev::isString(nameVal)) node->setName(ev::toUtf8(nameVal));

            std::string kindStr = strAtProp(opts, "type", "directional");
            if (kindStr == "point") node->setKind(scene::LightNode::Kind::Point);
            else if (kindStr == "spot") node->setKind(scene::LightNode::Kind::Spot);
            else node->setKind(scene::LightNode::Kind::Directional);

            Value posVal = ev::getProperty(opts, "position");
            bromath::Vec3 pos;
            if (readVec3FromValue(posVal, pos)) {
                node->setPosition(pos);
            } else {
                double x = numAtProp(opts, "x", node->position().x);
                double y = numAtProp(opts, "y", node->position().y);
                double z = numAtProp(opts, "z", node->position().z);
                node->setPosition((float)x, (float)y, (float)z);
            }

            Value dirVal = ev::getProperty(opts, "direction");
            bromath::Vec3 dir;
            if (readVec3FromValue(dirVal, dir)) {
                node->setDirection(dir);
            }

            Value colVal = ev::getProperty(opts, "color");
            float cr = 1, cg = 1, cb = 1, ca = 1;
            if (parseColorValue(colVal, cr, cg, cb, ca)) {
                node->setColor(cr, cg, cb);
            }

            Value iVal = ev::getProperty(opts, "intensity");
            if (ev::isNumber(iVal)) node->setIntensity(static_cast<float>(ev::toDouble(iVal)));

            Value rVal = ev::getProperty(opts, "range");
            if (ev::isNumber(rVal)) node->setRange(static_cast<float>(ev::toDouble(rVal)));

            Value iaVal = ev::getProperty(opts, "innerAngle");
            if (ev::isNumber(iaVal)) node->setInnerAngle(static_cast<float>(ev::toDouble(iaVal)));

            Value oaVal = ev::getProperty(opts, "outerAngle");
            if (ev::isNumber(oaVal)) node->setOuterAngle(static_cast<float>(ev::toDouble(oaVal)));

            Value csVal = ev::getProperty(opts, "castsShadow");
            if (!ev::isUndefined(csVal)) node->setCastsShadow(ev::toBool(csVal));

            Value sbVal = ev::getProperty(opts, "shadowBias");
            if (ev::isNumber(sbVal)) node->setShadowBias(static_cast<float>(ev::toDouble(sbVal)));

            Value snbVal = ev::getProperty(opts, "shadowNormalBias");
            if (ev::isNumber(snbVal)) node->setShadowNormalBias(static_cast<float>(ev::toDouble(snbVal)));

            Value ccVal = ev::getProperty(opts, "cascadeCount");
            if (ev::isNumber(ccVal)) node->setCascadeCount(static_cast<int>(ev::toDouble(ccVal)));

            Value cslVal = ev::getProperty(opts, "cascadeSplitLambda");
            if (ev::isNumber(cslVal)) node->setCascadeSplitLambda(static_cast<float>(ev::toDouble(cslVal)));
        }
        return wrapSceneNode(node, g);
    });
}

void installSceneNodeLights(ObjectBuilder& b) {
    b.accessor("kind", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (n && n->type() == scene::SceneNode::Type::Light) {
            auto* ln = static_cast<scene::LightNode*>(n);
            switch (ln->kind()) {
                case scene::LightNode::Kind::Point: return ev::fromUtf8("point");
                case scene::LightNode::Kind::Spot: return ev::fromUtf8("spot");
                case scene::LightNode::Kind::Directional:
                default: return ev::fromUtf8("directional");
            }
        }
        return ev::undefined();
    }, nullptr);

    b.accessor("direction",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Light) {
                return vec3ToValue(static_cast<scene::LightNode*>(n)->direction());
            }
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Light && !a.empty()) {
                bromath::Vec3 d;
                if (readVec3FromValue(a[0], d)) {
                    static_cast<scene::LightNode*>(n)->setDirection(d);
                }
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
                return hostArrayOf(4, [c](size_t i) {
                    return ev::fromDouble(c[i]);
                });
            }
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (!n || a.empty()) return ev::undefined();
            float r = 1, g = 1, b = 1, al = 1;
            if (parseColorValue(a[0], r, g, b, al)) {
                if (n->type() == scene::SceneNode::Type::Light) {
                    static_cast<scene::LightNode*>(n)->setColor(r, g, b);
                } else if (n->type() == scene::SceneNode::Type::Mesh) {
                    static_cast<scene::MeshNode*>(n)->setColor(r, g, b, al);
                }
            }
            return ev::undefined();
        });

    b.accessor("intensity",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Light) {
                return ev::fromDouble(static_cast<scene::LightNode*>(n)->intensity());
            }
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Light && !a.empty() && ev::isNumber(a[0])) {
                static_cast<scene::LightNode*>(n)->setIntensity(static_cast<float>(ev::toDouble(a[0])));
            }
            return ev::undefined();
        });

    b.accessor("range",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Light) {
                return ev::fromDouble(static_cast<scene::LightNode*>(n)->range());
            }
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Light && !a.empty() && ev::isNumber(a[0])) {
                static_cast<scene::LightNode*>(n)->setRange(static_cast<float>(ev::toDouble(a[0])));
            }
            return ev::undefined();
        });

    b.accessor("innerAngle",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Light) {
                return ev::fromDouble(static_cast<scene::LightNode*>(n)->innerAngle());
            }
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Light && !a.empty() && ev::isNumber(a[0])) {
                static_cast<scene::LightNode*>(n)->setInnerAngle(static_cast<float>(ev::toDouble(a[0])));
            }
            return ev::undefined();
        });

    b.accessor("outerAngle",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Light) {
                return ev::fromDouble(static_cast<scene::LightNode*>(n)->outerAngle());
            }
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Light && !a.empty() && ev::isNumber(a[0])) {
                static_cast<scene::LightNode*>(n)->setOuterAngle(static_cast<float>(ev::toDouble(a[0])));
            }
            return ev::undefined();
        });

    b.accessor("castsShadow",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (!n) return ev::fromBool(false);
            if (n->type() == scene::SceneNode::Type::Light) {
                return ev::fromBool(static_cast<scene::LightNode*>(n)->castsShadow());
            }
            if (n->type() == scene::SceneNode::Type::Mesh) {
                return ev::fromBool(static_cast<scene::MeshNode*>(n)->castsShadow());
            }
            return ev::fromBool(false);
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (!n || a.empty()) return ev::undefined();
            bool cs = ev::toBool(a[0]);
            if (n->type() == scene::SceneNode::Type::Light) {
                static_cast<scene::LightNode*>(n)->setCastsShadow(cs);
            } else if (n->type() == scene::SceneNode::Type::Mesh) {
                static_cast<scene::MeshNode*>(n)->setCastsShadow(cs);
            }
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
            if (n && n->type() == scene::SceneNode::Type::Mesh && !a.empty()) {
                static_cast<scene::MeshNode*>(n)->setReceivesShadow(ev::toBool(a[0]));
            }
            return ev::undefined();
        });

    b.accessor("shadowBias",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Light) {
                return ev::fromDouble(static_cast<scene::LightNode*>(n)->shadowBias());
            }
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Light && !a.empty() && ev::isNumber(a[0])) {
                static_cast<scene::LightNode*>(n)->setShadowBias(static_cast<float>(ev::toDouble(a[0])));
            }
            return ev::undefined();
        });

    b.accessor("shadowNormalBias",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Light) {
                return ev::fromDouble(static_cast<scene::LightNode*>(n)->shadowNormalBias());
            }
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Light && !a.empty() && ev::isNumber(a[0])) {
                static_cast<scene::LightNode*>(n)->setShadowNormalBias(static_cast<float>(ev::toDouble(a[0])));
            }
            return ev::undefined();
        });

    b.accessor("cascadeCount",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Light) {
                return ev::fromDouble(static_cast<scene::LightNode*>(n)->cascadeCount());
            }
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Light && !a.empty() && ev::isNumber(a[0])) {
                static_cast<scene::LightNode*>(n)->setCascadeCount(static_cast<int>(ev::toDouble(a[0])));
            }
            return ev::undefined();
        });

    b.accessor("cascadeSplitLambda",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Light) {
                return ev::fromDouble(static_cast<scene::LightNode*>(n)->cascadeSplitLambda());
            }
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Light && !a.empty() && ev::isNumber(a[0])) {
                static_cast<scene::LightNode*>(n)->setCascadeSplitLambda(static_cast<float>(ev::toDouble(a[0])));
            }
            return ev::undefined();
        });

    // Camera properties
    b.accessor("fov",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Camera)
                return ev::fromDouble(static_cast<scene::CameraNode*>(n)->fovDegrees());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Camera && !a.empty() && ev::isNumber(a[0]))
                static_cast<scene::CameraNode*>(n)->setFovDegrees(static_cast<float>(ev::toDouble(a[0])));
            return ev::undefined();
        });

    b.accessor("near",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Camera)
                return ev::fromDouble(static_cast<scene::CameraNode*>(n)->nearZ());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Camera && !a.empty() && ev::isNumber(a[0]))
                static_cast<scene::CameraNode*>(n)->setNearZ(static_cast<float>(ev::toDouble(a[0])));
            return ev::undefined();
        });

    b.accessor("far",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Camera)
                return ev::fromDouble(static_cast<scene::CameraNode*>(n)->farZ());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Camera && !a.empty() && ev::isNumber(a[0]))
                static_cast<scene::CameraNode*>(n)->setFarZ(static_cast<float>(ev::toDouble(a[0])));
            return ev::undefined();
        });

    b.accessor("aspect",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Camera)
                return ev::fromDouble(static_cast<scene::CameraNode*>(n)->aspect());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Camera && !a.empty() && ev::isNumber(a[0]))
                static_cast<scene::CameraNode*>(n)->setAspect(static_cast<float>(ev::toDouble(a[0])));
            return ev::undefined();
        });

    b.accessor("size",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Camera)
                return ev::fromDouble(static_cast<scene::CameraNode*>(n)->orthoHeight());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Camera && !a.empty() && ev::isNumber(a[0]))
                static_cast<scene::CameraNode*>(n)->setOrthoHeight(static_cast<float>(ev::toDouble(a[0])));
            return ev::undefined();
        });

    b.accessor("projection",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Camera)
                return ev::fromUtf8(static_cast<scene::CameraNode*>(n)->perspective() ? "perspective" : "orthographic");
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Camera && !a.empty() && ev::isString(a[0])) {
                std::string s = ev::toUtf8(a[0]);
                static_cast<scene::CameraNode*>(n)->setPerspective(s != "orthographic" && s != "ortho");
            }
            return ev::undefined();
        });
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
