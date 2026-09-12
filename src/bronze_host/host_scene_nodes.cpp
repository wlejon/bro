#if BRO_WITH_3D

#include "bronze_host/host_scene_internal.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/mesh_node.h"
#include "scene/physics_node.h"

#include <cmath>

namespace bro::bronze_host {

namespace {

double numAtProp(Value obj, const char* key, double defVal) {
    if (!ev::isObject(obj)) return defVal;
    Value v = ev::getProperty(obj, key);
    return ev::isNumber(v) ? ev::toDouble(v) : defVal;
}

}  // namespace

void installSceneNodeCore(ObjectBuilder& b) {
    // Identity
    b.accessor("id", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        return ev::fromDouble(n ? n->id() : 0);
    }, nullptr);

    b.accessor("name",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            return ev::fromUtf8(n ? n->name() : "");
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && !a.empty() && !ev::isObject(a[0])) {
                n->setName(ev::toUtf8(a[0]));
            }
            return ev::undefined();
        });

    // Visibility
    b.accessor("visible",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            return ev::fromBool(n ? n->visible() : false);
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && !a.empty()) {
                n->setVisible(ev::toBool(a[0]));
            }
            return ev::undefined();
        });

    b.accessor("childCount", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        return ev::fromDouble(n ? static_cast<double>(n->children().size()) : 0.0);
    }, nullptr);

    b.accessor("parent", [](Value self_, std::span<const Value>) {
        auto* c = sceneNodeCellOf(self_);
        auto* n = c ? c->node() : nullptr;
        if (!n || !n->parent()) return ev::null();
        return wrapSceneNode(n->parent(), c->graph());
    }, nullptr);

    b.accessor("children", [](Value self_, std::span<const Value>) {
        auto* c = sceneNodeCellOf(self_);
        auto* n = c ? c->node() : nullptr;
        if (!n) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        const auto& kids = n->children();
        auto* g = c->graph();
        return hostArrayOf(kids.size(), [&kids, g](size_t i) {
            return wrapSceneNode(kids[i], g);
        });
    }, nullptr);

    // Transform
    b.accessor("position",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (!n) return ev::undefined();
            return vec3ToValue(n->position());
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (!n || a.empty()) return ev::undefined();
            bromath::Vec3 p;
            if (readVec3FromValue(a[0], p)) {
                n->setPosition(p);
            }
            return ev::undefined();
        });

    b.accessor("x",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            return ev::fromDouble(n ? n->position().x : 0.0);
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && !a.empty()) {
                n->setPosition((float)ev::toDouble(a[0]), n->position().y, n->position().z);
            }
            return ev::undefined();
        });

    b.accessor("y",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            return ev::fromDouble(n ? n->position().y : 0.0);
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && !a.empty()) {
                n->setPosition(n->position().x, (float)ev::toDouble(a[0]), n->position().z);
            }
            return ev::undefined();
        });

    b.accessor("z",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            return ev::fromDouble(n ? n->position().z : 0.0);
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && !a.empty()) {
                n->setPosition(n->position().x, n->position().y, (float)ev::toDouble(a[0]));
            }
            return ev::undefined();
        });

    b.accessor("rotation",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            return ev::fromDouble(n ? n->rotationEuler().z : 0.0);
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && !a.empty()) {
                n->setRotationZ((float)ev::toDouble(a[0]));
            }
            return ev::undefined();
        });

    b.accessor("rotationX",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            return ev::fromDouble(n ? n->rotationEuler().x : 0.0);
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && !a.empty()) {
                const auto e = n->rotationEuler();
                n->setRotationEuler((float)ev::toDouble(a[0]), e.y, e.z);
            }
            return ev::undefined();
        });

    b.accessor("rotationY",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            return ev::fromDouble(n ? n->rotationEuler().y : 0.0);
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && !a.empty()) {
                const auto e = n->rotationEuler();
                n->setRotationEuler(e.x, (float)ev::toDouble(a[0]), e.z);
            }
            return ev::undefined();
        });

    b.accessor("rotationZ",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            return ev::fromDouble(n ? n->rotationEuler().z : 0.0);
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && !a.empty()) {
                const auto e = n->rotationEuler();
                n->setRotationEuler(e.x, e.y, (float)ev::toDouble(a[0]));
            }
            return ev::undefined();
        });

    b.accessor("quaternion",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (!n) return ev::undefined();
            return quatToValue(n->rotation());
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (!n || a.empty()) return ev::undefined();
            bromath::Quat q;
            if (readQuatFromValue(a[0], q)) {
                n->setRotation(bromath::qnorm(q));
            }
            return ev::undefined();
        });

    b.accessor("scale",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (!n) return ev::undefined();
            return vec3ToValue(n->scale());
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (!n || a.empty()) return ev::undefined();
            if (ev::isNumber(a[0])) {
                float s = (float)ev::toDouble(a[0]);
                n->setScale(s, s, s);
            } else if (ev::isObject(a[0])) {
                bromath::Vec3 cur = n->scale();
                Value e0 = ev::getElement(a[0], 0);
                Value e1 = ev::getElement(a[0], 1);
                Value e2 = ev::getElement(a[0], 2);
                if (ev::isNumber(e0)) cur.x = (float)ev::toDouble(e0);
                if (ev::isNumber(e1)) cur.y = (float)ev::toDouble(e1);
                if (ev::isNumber(e2)) cur.z = (float)ev::toDouble(e2);
                n->setScale(cur.x, cur.y, cur.z);
            }
            return ev::undefined();
        });

    b.accessor("scaleX",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            return ev::fromDouble(n ? n->scale().x : 1.0);
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && !a.empty()) n->setScale((float)ev::toDouble(a[0]), n->scale().y, n->scale().z);
            return ev::undefined();
        });

    b.accessor("scaleY",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            return ev::fromDouble(n ? n->scale().y : 1.0);
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && !a.empty()) n->setScale(n->scale().x, (float)ev::toDouble(a[0]), n->scale().z);
            return ev::undefined();
        });

    b.accessor("scaleZ",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            return ev::fromDouble(n ? n->scale().z : 1.0);
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && !a.empty()) n->setScale(n->scale().x, n->scale().y, (float)ev::toDouble(a[0]));
            return ev::undefined();
        });

    // Type
    b.accessor("type", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (!n) return ev::undefined();
        switch (n->type()) {
            case scene::SceneNode::Type::Mesh:
                return ev::fromUtf8(static_cast<scene::MeshNode*>(n)->asSkinnedMesh() ? "skinnedMesh" : "mesh");
            case scene::SceneNode::Type::InstancedMesh: return ev::fromUtf8("instancedMesh");
            case scene::SceneNode::Type::Light:          return ev::fromUtf8("light");
            case scene::SceneNode::Type::Shape:          return ev::fromUtf8("shape");
            case scene::SceneNode::Type::Sprite:         return ev::fromUtf8("sprite");
            case scene::SceneNode::Type::Physics:        return ev::fromUtf8("physics");
            case scene::SceneNode::Type::Html:           return ev::fromUtf8("html");
            case scene::SceneNode::Type::GaussianSplat:  return ev::fromUtf8("gaussianSplat");
            case scene::SceneNode::Type::Particles:      return ev::fromUtf8("particles");
            case scene::SceneNode::Type::Particles3D:    return ev::fromUtf8("particles3d");
            case scene::SceneNode::Type::Camera:         return ev::fromUtf8("camera");
            case scene::SceneNode::Type::Decal:          return ev::fromUtf8("decal");
            case scene::SceneNode::Type::ReflectionProbe:return ev::fromUtf8("reflectionProbe");
            case scene::SceneNode::Type::Base:           return ev::fromUtf8("group");
            default: break;
        }
        return ev::undefined();
    }, nullptr);

    // World anchor + billboard
    b.accessor("worldAnchor",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (!n || !n->hasWorldAnchor()) return ev::null();
            return vec3ToValue(n->worldAnchor());
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (!n || a.empty()) return ev::undefined();
            if (ev::isNull(a[0]) || ev::isUndefined(a[0])) {
                n->clearWorldAnchor();
            } else {
                bromath::Vec3 p;
                if (readVec3FromValue(a[0], p)) n->setWorldAnchor(p);
            }
            return ev::undefined();
        });

    b.accessor("billboard",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (!n) return ev::undefined();
            return ev::fromUtf8(n->billboardMode() == scene::SceneNode::BillboardMode::YLock ? "ylock" : "full");
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (!n || a.empty()) return ev::undefined();
            std::string s = ev::toUtf8(a[0]);
            if (s == "ylock" || s == "yLock" || s == "y-lock") {
                n->setBillboardMode(scene::SceneNode::BillboardMode::YLock);
            } else {
                n->setBillboardMode(scene::SceneNode::BillboardMode::Full);
            }
            return ev::undefined();
        });

    // Visibility range
    b.accessor("visibilityRange",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (!n || !n->hasVisibilityRange()) return ev::null();
            ObjectBuilder r;
            r.set("begin", ev::fromDouble(n->visibilityRangeBegin()));
            r.set("end", ev::fromDouble(n->visibilityRangeEnd()));
            r.set("margin", ev::fromDouble(n->visibilityRangeMargin()));
            return r.get();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (!n || a.empty()) return ev::undefined();
            if (ev::isNull(a[0]) || ev::isUndefined(a[0])) {
                n->clearVisibilityRange();
            } else if (ev::isObject(a[0])) {
                float b_ = (float)numAtProp(a[0], "begin", 0.0);
                float e_ = (float)numAtProp(a[0], "end", 1e30);
                float m_ = (float)numAtProp(a[0], "margin", 0.0);
                n->setVisibilityRange(b_, e_, m_);
            }
            return ev::undefined();
        });

    // Physics properties
    b.accessor("autoSync",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Physics)
                return ev::fromBool(static_cast<scene::PhysicsNode*>(n)->autoSync());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Physics && !a.empty())
                static_cast<scene::PhysicsNode*>(n)->setAutoSync(ev::toBool(a[0]));
            return ev::undefined();
        });

    b.accessor("pixelsPerUnit",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Physics)
                return ev::fromDouble(static_cast<scene::PhysicsNode*>(n)->pixelsPerUnit());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Physics && !a.empty())
                static_cast<scene::PhysicsNode*>(n)->setPixelsPerUnit((float)ev::toDouble(a[0]));
            return ev::undefined();
        });

    b.accessor("bodyId", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (n && n->type() == scene::SceneNode::Type::Physics) {
            auto* p = static_cast<scene::PhysicsNode*>(n);
            if (p->hasBody())
                return ev::fromDouble(p->bodyId().GetIndexAndSequenceNumber());
            return ev::null();
        }
        return ev::undefined();
    }, nullptr);

    // Methods
    auto addFn = [](Value self_, std::span<const Value> a) {
        auto* parent = sceneNodeOf(self_);
        if (!parent || a.empty()) return self_;
        auto* child = sceneNodeOf(a[0]);
        if (!child) return ev::throwTypeError("argument must be a SceneNode");
        parent->addChild(child);
        return self_;
    };
    b.def("add", 1, addFn);
    b.def("addChild", 1, addFn);

    auto remFn = [](Value self_, std::span<const Value> a) {
        auto* parent = sceneNodeOf(self_);
        if (!parent || a.empty()) return ev::undefined();
        auto* child = sceneNodeOf(a[0]);
        if (child) parent->removeChild(child);
        return ev::undefined();
    };
    b.def("remove", 1, remFn);
    b.def("removeChild", 1, remFn);

    b.def("destroy", 0, [](Value self_, std::span<const Value>) {
        auto* c = sceneNodeCellOf(self_);
        if (c) {
            if (auto* g = c->graph()) {
                if (auto* n = c->node()) {
                    auto tok = c->token.lock();
                    if (tok) {
                        n->traverse([&](scene::SceneNode* kid) {
                            pruneSceneNodeWrapper(tok.get(), kid->id());
                        });
                    }
                    g->destroyNode(n);
                }
            }
        }
        return ev::undefined();
    });

    b.def("lookAt", 3, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n || a.empty()) return self_;
        bromath::Vec3 t;
        if (a.size() >= 3 && ev::isNumber(a[0]) && ev::isNumber(a[1]) && ev::isNumber(a[2])) {
            t = {(float)ev::toDouble(a[0]), (float)ev::toDouble(a[1]), (float)ev::toDouble(a[2])};
        } else if (readVec3FromValue(a[0], t)) {
            // ok
        } else {
            return ev::throwTypeError("lookAt: expected (x, y, z) or ([x, y, z])");
        }
        n->lookAt(t);
        return self_;
    });

    b.def("localToWorld", 3, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n || a.size() < 2) return ev::undefined();
        float z = a.size() > 2 ? (float)ev::toDouble(a[2]) : 0.0f;
        auto wp = n->localToWorld({(float)ev::toDouble(a[0]), (float)ev::toDouble(a[1]), z});
        ObjectBuilder r;
        r.set("x", ev::fromDouble(wp.x));
        r.set("y", ev::fromDouble(wp.y));
        r.set("z", ev::fromDouble(wp.z));
        return r.get();
    });

    b.def("setPosition", 3, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n) return self_;
        if (a.size() >= 3) {
            n->setPosition((float)ev::toDouble(a[0]), (float)ev::toDouble(a[1]), (float)ev::toDouble(a[2]));
        } else if (!a.empty()) {
            bromath::Vec3 p;
            if (readVec3FromValue(a[0], p)) n->setPosition(p);
        }
        return self_;
    });

    b.def("setRotation", 4, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n) return self_;
        if (a.size() >= 4) {
            bromath::Quat q{(float)ev::toDouble(a[0]), (float)ev::toDouble(a[1]), (float)ev::toDouble(a[2]), (float)ev::toDouble(a[3])};
            n->setRotation(bromath::qnorm(q));
        } else if (!a.empty()) {
            bromath::Quat q;
            if (readQuatFromValue(a[0], q)) n->setRotation(bromath::qnorm(q));
        }
        return self_;
    });

    b.def("setScale", 3, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n) return self_;
        if (a.size() >= 3) {
            n->setScale((float)ev::toDouble(a[0]), (float)ev::toDouble(a[1]), (float)ev::toDouble(a[2]));
        } else if (a.size() == 1 && ev::isNumber(a[0])) {
            float s = (float)ev::toDouble(a[0]);
            n->setScale(s, s, s);
        } else if (!a.empty()) {
            bromath::Vec3 s;
            if (readVec3FromValue(a[0], s)) n->setScale(s);
        }
        return self_;
    });

    b.def("syncToPhysics", 0, [](Value self_, std::span<const Value>) {
        auto* c = sceneNodeCellOf(self_);
        if (c && c->node() && c->node()->type() == scene::SceneNode::Type::Physics) {
            auto* pn = static_cast<scene::PhysicsNode*>(c->node());
            if (c->graph() && c->graph()->physicsWorld())
                pn->syncToPhysics(c->graph()->physicsWorld());
        }
        return ev::undefined();
    });
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
