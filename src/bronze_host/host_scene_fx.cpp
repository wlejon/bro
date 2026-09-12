#if BRO_WITH_3D

#include "bronze_host/host_scene_internal.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/particle_node.h"
#include "scene/particles3d_node.h"
#include "scene/decal_node.h"
#include "scene/reflection_probe_node.h"
#include "scene/gaussian_splat_node.h"
#include "bromesh/io/splat_ply.h"
#include "util/log.h"

#include <cmath>
#include <span>
#include <string>
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

bool boolAtProp(Value obj, const char* key, bool defVal) {
    if (!ev::isObject(obj)) return defVal;
    Value v = ev::getProperty(obj, key);
    return !ev::isUndefined(v) ? ev::toBool(v) : defVal;
}

void applyWorldAnchorAndBillboard(Value opts, scene::SceneNode* node) {
    if (!node || !ev::isObject(opts)) return;
    Value wa = ev::getProperty(opts, "worldAnchor");
    if (ev::isObject(wa)) {
        bromath::Vec3 a;
        if (readVec3FromValue(wa, a)) node->setWorldAnchor(a);
    }
    Value bb = ev::getProperty(opts, "billboard");
    if (ev::isString(bb)) {
        std::string mode = ev::toUtf8(bb);
        if (mode == "ylock" || mode == "yLock" || mode == "y-lock") {
            node->setBillboardMode(scene::SceneNode::BillboardMode::YLock);
        } else {
            node->setBillboardMode(scene::SceneNode::BillboardMode::Full);
        }
    }
}

bool extractTextureObj(Value texObj, std::vector<uint8_t>& outBytes, int& outW, int& outH) {
    if (!ev::isObject(texObj)) return false;
    outW = static_cast<int>(numAtProp(texObj, "width", 0));
    outH = static_cast<int>(numAtProp(texObj, "height", 0));
    Value dVal = ev::getProperty(texObj, "data");
    ev::TypedArrayInfo info = ev::typedArrayInfo(dVal);
    if (!info || !info.data || info.byteLength < static_cast<size_t>(outW * outH * 4)) return false;
    outBytes.assign(reinterpret_cast<const uint8_t*>(info.data), reinterpret_cast<const uint8_t*>(info.data) + info.byteLength);
    return true;
}

}  // namespace

void installSceneGraphFx(ObjectBuilder& b) {
    b.def("createParticles", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::undefined();
        auto* node = g->createParticles();
        g->root()->addChild(node);
        if (!a.empty() && ev::isObject(a[0])) {
            Value opts = a[0];
            Value nameVal = ev::getProperty(opts, "name");
            if (ev::isString(nameVal)) node->setName(ev::toUtf8(nameVal));

            double maxP = numAtProp(opts, "maxParticles", 100);
            node->setMaxParticles(static_cast<int>(maxP));

            double rate = numAtProp(opts, "emissionRate", 10);
            if (!ev::isUndefined(ev::getProperty(opts, "rate")))
                rate = numAtProp(opts, "rate", rate);
            node->setRate((float)rate);

            double lmin = numAtProp(opts, "lifetimeMin", 1);
            double lmax = numAtProp(opts, "lifetimeMax", 2);
            node->setLifetime((float)lmin, (float)lmax);

            double smin = numAtProp(opts, "speedMin", 10);
            double smax = numAtProp(opts, "speedMax", 20);
            float speed = static_cast<float>((smin + smax) * 0.5);
            float speedSpread = static_cast<float>(std::abs(smax - smin) * 0.5);
            node->setVelocity(-90.0f, 360.0f, speed, speedSpread);

            double szmin = numAtProp(opts, "sizeMin", 4);
            double szmax = numAtProp(opts, "sizeMax", 8);
            node->setSize((float)szmin, (float)szmax);

            float sr = 1, sg = 1, sb = 1, sa = 1;
            float er = 1, eg = 1, eb = 1, ea = 0;
            parseColorValue(ev::getProperty(opts, "startColor"), sr, sg, sb, sa);
            parseColorValue(ev::getProperty(opts, "endColor"), er, eg, eb, ea);
            node->setColors(bromath::Color{sr, sg, sb, sa}, bromath::Color{er, eg, eb, ea});

            applyWorldAnchorAndBillboard(opts, node);

            double x = numAtProp(opts, "x", 0);
            double y = numAtProp(opts, "y", 0);
            double z = numAtProp(opts, "z", 0);
            node->setPosition((float)x, (float)y, (float)z);
        }
        return wrapSceneNode(node, g);
    });

    b.def("createParticles3D", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::undefined();
        auto* node = g->createParticles3D();
        g->root()->addChild(node);
        if (!a.empty() && ev::isObject(a[0])) {
            Value opts = a[0];
            Value nameVal = ev::getProperty(opts, "name");
            if (ev::isString(nameVal)) node->setName(ev::toUtf8(nameVal));

            double cap = numAtProp(opts, "capacity", 256);
            if (!ev::isUndefined(ev::getProperty(opts, "maxParticles")))
                cap = numAtProp(opts, "maxParticles", cap);
            node->setMaxParticles(static_cast<int>(cap));

            double rate = numAtProp(opts, "rate", 32);
            node->setRate((float)rate);

            double lmin = numAtProp(opts, "lifetimeMin", 1);
            double lmax = numAtProp(opts, "lifetimeMax", 2);
            node->setLifetime((float)lmin, (float)lmax);

            double sz0 = numAtProp(opts, "sizeStart", 0.1);
            double sz1 = numAtProp(opts, "sizeEnd", 0.2);
            node->setSize((float)sz0, (float)sz1);

            double soft = numAtProp(opts, "softness", 0);
            node->setSoftness((float)soft);

            Value colVal = ev::getProperty(opts, "color");
            float cr = 1, cg = 1, cb = 1, ca = 1;
            if (parseColorValue(colVal, cr, cg, cb, ca)) {
                node->setColors(bromath::Color{cr, cg, cb, ca}, bromath::Color{cr, cg, cb, 0.0f});
            }

            double dur = numAtProp(opts, "duration", 0);
            bool loop = boolAtProp(opts, "loop", true);
            node->setDuration((float)dur, loop);

            Value fnVal = ev::getProperty(opts, "onFinished");
            if (ev::isObject(fnVal)) {
                auto fnRef = std::make_shared<ev::Persistent>(fnVal);
                node->setOnFinished([fnRef]() {
                    if (fnRef && ev::isObject(fnRef->get())) {
                        ev::call(fnRef->get(), ev::undefined(), {});
                    }
                });
            }

            Value burstVal = ev::getProperty(opts, "burst");
            if (ev::isNumber(burstVal)) node->burst(static_cast<int>(ev::toDouble(burstVal)));

            double x = numAtProp(opts, "x", 0);
            double y = numAtProp(opts, "y", 0);
            double z = numAtProp(opts, "z", 0);
            node->setPosition((float)x, (float)y, (float)z);
        }
        return wrapSceneNode(node, g);
    });

    b.def("createDecal", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::undefined();
        auto* node = g->createDecal();
        g->root()->addChild(node);
        if (!a.empty() && ev::isObject(a[0])) {
            Value opts = a[0];
            Value nameVal = ev::getProperty(opts, "name");
            if (ev::isString(nameVal)) node->setName(ev::toUtf8(nameVal));

            // Position
            bromath::Vec3 pos = node->position();
            if (readVec3FromValue(ev::getProperty(opts, "position"), pos)) {
                node->setPosition(pos);
            }
            Value xVal = ev::getProperty(opts, "x");
            if (ev::isNumber(xVal)) node->setPosition(static_cast<float>(ev::toDouble(xVal)), node->position().y, node->position().z);
            Value yVal = ev::getProperty(opts, "y");
            if (ev::isNumber(yVal)) node->setPosition(node->position().x, static_cast<float>(ev::toDouble(yVal)), node->position().z);
            Value zVal = ev::getProperty(opts, "z");
            if (ev::isNumber(zVal)) node->setPosition(node->position().x, node->position().y, static_cast<float>(ev::toDouble(zVal)));

            // Size / scale
            Value szVal = ev::getProperty(opts, "size");
            bromath::Vec3 sz;
            if (readVec3FromValue(szVal, sz)) {
                node->setScale(sz);
            } else if (ev::isNumber(szVal)) {
                float s = static_cast<float>(ev::toDouble(szVal));
                node->setScale(s, s, s);
            } else {
                double sx = numAtProp(opts, "sizeX", 1);
                double sy = numAtProp(opts, "sizeY", 1);
                double szD = numAtProp(opts, "sizeZ", 1);
                node->setScale(static_cast<float>(sx), static_cast<float>(sy), static_cast<float>(szD));
            }

            Value scVal = ev::getProperty(opts, "scale");
            bromath::Vec3 sc;
            if (readVec3FromValue(scVal, sc)) {
                node->setScale(sc);
            } else if (ev::isNumber(scVal)) {
                float s = static_cast<float>(ev::toDouble(scVal));
                node->setScale(s, s, s);
            }

            // Rotation
            bromath::Quat quat;
            if (readQuatFromValue(ev::getProperty(opts, "quaternion"), quat)) {
                node->setRotation(bromath::qnorm(quat));
            } else {
                bromath::Vec3 rot;
                if (readVec3FromValue(ev::getProperty(opts, "rotation"), rot)) {
                    node->setRotationEuler(rot.x, rot.y, rot.z);
                }
            }
            Value rxVal = ev::getProperty(opts, "rotationX");
            if (ev::isNumber(rxVal)) {
                auto e = node->rotationEuler();
                node->setRotationEuler(static_cast<float>(ev::toDouble(rxVal)), e.y, e.z);
            }
            Value ryVal = ev::getProperty(opts, "rotationY");
            if (ev::isNumber(ryVal)) {
                auto e = node->rotationEuler();
                node->setRotationEuler(e.x, static_cast<float>(ev::toDouble(ryVal)), e.z);
            }
            Value rzVal = ev::getProperty(opts, "rotationZ");
            if (ev::isNumber(rzVal)) {
                auto e = node->rotationEuler();
                node->setRotationEuler(e.x, e.y, static_cast<float>(ev::toDouble(rzVal)));
            }

            // Modulate
            Value modVal = ev::getProperty(opts, "modulate");
            float mr = 1, mg = 1, mb = 1, ma = 1;
            if (parseColorValue(modVal, mr, mg, mb, ma)) node->setModulate(mr, mg, mb, ma);

            // Emission strength
            Value emVal = ev::getProperty(opts, "emissionStrength");
            if (ev::isNumber(emVal)) {
                node->setEmissionStrength(static_cast<float>(ev::toDouble(emVal)));
            }

            // Fades
            Value ufVal = ev::getProperty(opts, "upperFade");
            if (ev::isNumber(ufVal)) node->setUpperFade(static_cast<float>(ev::toDouble(ufVal)));
            Value lfVal = ev::getProperty(opts, "lowerFade");
            if (ev::isNumber(lfVal)) node->setLowerFade(static_cast<float>(ev::toDouble(lfVal)));
            Value nfVal = ev::getProperty(opts, "normalFade");
            if (ev::isNumber(nfVal)) node->setNormalFade(static_cast<float>(ev::toDouble(nfVal)));

            // Priority
            Value priVal = ev::getProperty(opts, "renderPriority");
            if (ev::isNumber(priVal)) node->setRenderPriority(static_cast<int>(ev::toDouble(priVal)));

            // Textures
            Value texVal = ev::getProperty(opts, "texture");
            if (ev::isUndefined(texVal)) texVal = ev::getProperty(opts, "albedoTexture");
            std::vector<uint8_t> bytes;
            int tw = 0, th = 0;
            if (extractTextureObj(texVal, bytes, tw, th)) {
                node->setAlbedoTexture(tw, th, bytes.data());
            }

            Value emTexVal = ev::getProperty(opts, "emissionTexture");
            std::vector<uint8_t> emBytes;
            int ew = 0, eh = 0;
            if (extractTextureObj(emTexVal, emBytes, ew, eh)) {
                node->setEmissionTexture(ew, eh, emBytes.data());
            }
        }
        return wrapSceneNode(node, g);
    });

    b.def("createReflectionProbe", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::undefined();
        auto* node = g->createReflectionProbe();
        g->root()->addChild(node);
        if (!a.empty() && ev::isObject(a[0])) {
            Value opts = a[0];
            Value nameVal = ev::getProperty(opts, "name");
            if (ev::isString(nameVal)) node->setName(ev::toUtf8(nameVal));

            double res = numAtProp(opts, "resolution", 128);
            node->setResolution(static_cast<int>(res));

            bool bp = boolAtProp(opts, "boxProjection", true);
            node->setBoxProjection(bp);

            Value szVal = ev::getProperty(opts, "size");
            bromath::Vec3 sz;
            if (readVec3FromValue(szVal, sz)) {
                node->setScale(sz);
            } else if (ev::isNumber(szVal)) {
                float s = static_cast<float>(ev::toDouble(szVal));
                node->setScale(s, s, s);
            } else {
                double sx = numAtProp(opts, "sizeX", 10);
                double sy = numAtProp(opts, "sizeY", 10);
                double szD = numAtProp(opts, "sizeZ", 10);
                node->setScale(static_cast<float>(sx), static_cast<float>(sy), static_cast<float>(szD));
            }

            double pri = numAtProp(opts, "priority", 0);
            node->setPriority(static_cast<int>(pri));

            bool interior = boolAtProp(opts, "interior", false);
            node->setInterior(interior);

            std::string um = strAtProp(opts, "updateMode", "once");
            if (um == "manual") node->setUpdateMode(scene::ReflectionProbeNode::UpdateMode::Manual);
            else node->setUpdateMode(scene::ReflectionProbeNode::UpdateMode::Once);

            double x = numAtProp(opts, "x", 0);
            double y = numAtProp(opts, "y", 0);
            double z = numAtProp(opts, "z", 0);
            node->setPosition((float)x, (float)y, (float)z);
        }
        return wrapSceneNode(node, g);
    });

    b.def("createGaussianSplat", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::undefined();
        auto* node = g->createGaussianSplat();
        g->root()->addChild(node);
        if (!a.empty() && ev::isObject(a[0])) {
            Value opts = a[0];
            Value nameVal = ev::getProperty(opts, "name");
            if (ev::isString(nameVal)) node->setName(ev::toUtf8(nameVal));

            Value plyVal = ev::getProperty(opts, "ply");
            if (!ev::isString(plyVal)) plyVal = ev::getProperty(opts, "path");
            if (ev::isString(plyVal)) {
                std::string path = ev::toUtf8(plyVal);
                bromesh::GaussianSplatCloud cloud = bromesh::loadSplatPLY(path);
                node->setCloud(std::move(cloud));
            }

            Value cloudVal = ev::getProperty(opts, "cloud");
            if (ev::isObject(cloudVal)) {
                bromesh::GaussianSplatCloud c;
                auto extractF32 = [](Value v, std::vector<float>& dst) {
                    ev::TypedArrayInfo info = ev::typedArrayInfo(v);
                    if (info && info.data && info.byteLength > 0) {
                        size_t count = info.byteLength / sizeof(float);
                        const float* fp = reinterpret_cast<const float*>(info.data);
                        dst.assign(fp, fp + count);
                    }
                };
                extractF32(ev::getProperty(cloudVal, "positions"), c.positions);
                extractF32(ev::getProperty(cloudVal, "scales"), c.scales);
                extractF32(ev::getProperty(cloudVal, "rotations"), c.rotations);
                extractF32(ev::getProperty(cloudVal, "opacities"), c.opacities);
                extractF32(ev::getProperty(cloudVal, "sh"), c.sh);
                c.shDegree = static_cast<int>(numAtProp(cloudVal, "shDegree", 0));
                node->setCloud(std::move(c));
            }

            double x = numAtProp(opts, "x", 0);
            double y = numAtProp(opts, "y", 0);
            double z = numAtProp(opts, "z", 0);
            node->setPosition((float)x, (float)y, (float)z);

            Value scVal = ev::getProperty(opts, "scale");
            if (ev::isNumber(scVal)) {
                float s = static_cast<float>(ev::toDouble(scVal));
                node->setScale(s, s, s);
            }
        }
        return wrapSceneNode(node, g);
    });
}

void installSceneNodeFx(ObjectBuilder& b) {
    // Decal properties
    b.accessor("modulate",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Decal) {
                const float* m = static_cast<scene::DecalNode*>(n)->modulate();
                return hostArrayOf(4, [m](size_t i) {
                    return ev::fromDouble(m[i]);
                });
            }
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Decal && !a.empty()) {
                float r = 1, g = 1, b = 1, al = 1;
                if (parseColorValue(a[0], r, g, b, al)) {
                    static_cast<scene::DecalNode*>(n)->setModulate(r, g, b, al);
                }
            }
            return ev::undefined();
        });

    b.accessor("emissionStrength",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Decal)
                return ev::fromDouble(static_cast<scene::DecalNode*>(n)->emissionStrength());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Decal && !a.empty() && ev::isNumber(a[0]))
                static_cast<scene::DecalNode*>(n)->setEmissionStrength(static_cast<float>(ev::toDouble(a[0])));
            return ev::undefined();
        });

    b.accessor("upperFade",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Decal)
                return ev::fromDouble(static_cast<scene::DecalNode*>(n)->upperFade());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Decal && !a.empty() && ev::isNumber(a[0]))
                static_cast<scene::DecalNode*>(n)->setUpperFade(static_cast<float>(ev::toDouble(a[0])));
            return ev::undefined();
        });

    b.accessor("lowerFade",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Decal)
                return ev::fromDouble(static_cast<scene::DecalNode*>(n)->lowerFade());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Decal && !a.empty() && ev::isNumber(a[0]))
                static_cast<scene::DecalNode*>(n)->setLowerFade(static_cast<float>(ev::toDouble(a[0])));
            return ev::undefined();
        });

    b.accessor("normalFade",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Decal)
                return ev::fromDouble(static_cast<scene::DecalNode*>(n)->normalFade());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Decal && !a.empty() && ev::isNumber(a[0]))
                static_cast<scene::DecalNode*>(n)->setNormalFade(static_cast<float>(ev::toDouble(a[0])));
            return ev::undefined();
        });

    b.accessor("renderPriority",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Decal)
                return ev::fromDouble(static_cast<scene::DecalNode*>(n)->renderPriority());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Decal && !a.empty() && ev::isNumber(a[0]))
                static_cast<scene::DecalNode*>(n)->setRenderPriority(static_cast<int>(ev::toDouble(a[0])));
            return ev::undefined();
        });

    b.def("setBaseColorTexture", 1, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (n && n->type() == scene::SceneNode::Type::Decal && !a.empty()) {
            std::vector<uint8_t> bytes;
            int tw = 0, th = 0;
            if (extractTextureObj(a[0], bytes, tw, th)) {
                static_cast<scene::DecalNode*>(n)->setAlbedoTexture(tw, th, bytes.data());
            }
        }
        return ev::undefined();
    });

    b.def("setAlbedoTexture", 1, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (n && n->type() == scene::SceneNode::Type::Decal && !a.empty()) {
            std::vector<uint8_t> bytes;
            int tw = 0, th = 0;
            if (extractTextureObj(a[0], bytes, tw, th)) {
                static_cast<scene::DecalNode*>(n)->setAlbedoTexture(tw, th, bytes.data());
            }
        }
        return ev::undefined();
    });

    b.def("setEmissionTexture", 1, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (n && n->type() == scene::SceneNode::Type::Decal && !a.empty()) {
            std::vector<uint8_t> bytes;
            int tw = 0, th = 0;
            if (extractTextureObj(a[0], bytes, tw, th)) {
                static_cast<scene::DecalNode*>(n)->setEmissionTexture(tw, th, bytes.data());
            }
        }
        return ev::undefined();
    });

    // Reflection Probe properties
    b.accessor("boxProjection",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::ReflectionProbe)
                return ev::fromBool(static_cast<scene::ReflectionProbeNode*>(n)->boxProjection());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::ReflectionProbe && !a.empty())
                static_cast<scene::ReflectionProbeNode*>(n)->setBoxProjection(ev::toBool(a[0]));
            return ev::undefined();
        });

    b.accessor("interior",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::ReflectionProbe)
                return ev::fromBool(static_cast<scene::ReflectionProbeNode*>(n)->interior());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::ReflectionProbe && !a.empty())
                static_cast<scene::ReflectionProbeNode*>(n)->setInterior(ev::toBool(a[0]));
            return ev::undefined();
        });

    b.accessor("priority",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::ReflectionProbe)
                return ev::fromDouble(static_cast<scene::ReflectionProbeNode*>(n)->priority());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::ReflectionProbe && !a.empty() && ev::isNumber(a[0]))
                static_cast<scene::ReflectionProbeNode*>(n)->setPriority(static_cast<int>(ev::toDouble(a[0])));
            return ev::undefined();
        });

    b.accessor("resolution",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::ReflectionProbe)
                return ev::fromDouble(static_cast<scene::ReflectionProbeNode*>(n)->resolution());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::ReflectionProbe && !a.empty() && ev::isNumber(a[0]))
                static_cast<scene::ReflectionProbeNode*>(n)->setResolution(static_cast<int>(ev::toDouble(a[0])));
            return ev::undefined();
        });

    b.accessor("updateMode",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::ReflectionProbe) {
                auto m = static_cast<scene::ReflectionProbeNode*>(n)->updateMode();
                return ev::fromUtf8(m == scene::ReflectionProbeNode::UpdateMode::Manual ? "manual" : "once");
            }
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::ReflectionProbe && !a.empty() && ev::isString(a[0])) {
                std::string s = ev::toUtf8(a[0]);
                static_cast<scene::ReflectionProbeNode*>(n)->setUpdateMode(
                    s == "manual" ? scene::ReflectionProbeNode::UpdateMode::Manual : scene::ReflectionProbeNode::UpdateMode::Once);
            }
            return ev::undefined();
        });

    b.def("capture", 0, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (n && n->type() == scene::SceneNode::Type::ReflectionProbe) {
            static_cast<scene::ReflectionProbeNode*>(n)->requestCapture();
        }
        return ev::undefined();
    });

    // Particle & Particles3D properties
    b.accessor("particleCount", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (!n) return ev::fromDouble(0.0);
        if (n->type() == scene::SceneNode::Type::Particles)
            return ev::fromDouble(static_cast<scene::ParticleNode*>(n)->liveCount());
        if (n->type() == scene::SceneNode::Type::Particles3D)
            return ev::fromDouble(static_cast<scene::Particles3DNode*>(n)->liveCount());
        return ev::fromDouble(0.0);
    }, nullptr);

    b.accessor("liveCount", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (!n) return ev::fromDouble(0.0);
        if (n->type() == scene::SceneNode::Type::Particles3D)
            return ev::fromDouble(static_cast<scene::Particles3DNode*>(n)->liveCount());
        if (n->type() == scene::SceneNode::Type::Particles)
            return ev::fromDouble(static_cast<scene::ParticleNode*>(n)->liveCount());
        return ev::fromDouble(0.0);
    }, nullptr);

    b.accessor("rate",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (!n) return ev::undefined();
            if (n->type() == scene::SceneNode::Type::Particles)
                return ev::fromDouble(static_cast<scene::ParticleNode*>(n)->rate());
            if (n->type() == scene::SceneNode::Type::Particles3D)
                return ev::fromDouble(static_cast<scene::Particles3DNode*>(n)->rate());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (!n || a.empty() || !ev::isNumber(a[0])) return ev::undefined();
            float r = static_cast<float>(ev::toDouble(a[0]));
            if (n->type() == scene::SceneNode::Type::Particles)
                static_cast<scene::ParticleNode*>(n)->setRate(r);
            else if (n->type() == scene::SceneNode::Type::Particles3D)
                static_cast<scene::Particles3DNode*>(n)->setRate(r);
            return ev::undefined();
        });

    b.accessor("softness",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Particles3D)
                return ev::fromDouble(static_cast<scene::Particles3DNode*>(n)->softness());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Particles3D && !a.empty() && ev::isNumber(a[0]))
                static_cast<scene::Particles3DNode*>(n)->setSoftness(static_cast<float>(ev::toDouble(a[0])));
            return ev::undefined();
        });

    b.accessor("onFinished",
        [](Value self_, std::span<const Value>) { return ev::undefined(); },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Particles3D && !a.empty() && ev::isObject(a[0])) {
                auto fnRef = std::make_shared<ev::Persistent>(a[0]);
                static_cast<scene::Particles3DNode*>(n)->setOnFinished([fnRef]() {
                    if (fnRef && ev::isObject(fnRef->get())) {
                        ev::call(fnRef->get(), ev::undefined(), {});
                    }
                });
            }
            return ev::undefined();
        });

    b.def("burst", 1, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n || a.empty() || !ev::isNumber(a[0])) return ev::undefined();
        int count = static_cast<int>(ev::toDouble(a[0]));
        if (n->type() == scene::SceneNode::Type::Particles)
            static_cast<scene::ParticleNode*>(n)->burst(count);
        else if (n->type() == scene::SceneNode::Type::Particles3D)
            static_cast<scene::Particles3DNode*>(n)->burst(count);
        return ev::undefined();
    });

    b.def("clear", 0, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n) return ev::undefined();
        if (n->type() == scene::SceneNode::Type::Particles)
            static_cast<scene::ParticleNode*>(n)->clear();
        else if (n->type() == scene::SceneNode::Type::Particles3D)
            static_cast<scene::Particles3DNode*>(n)->clear();
        return ev::undefined();
    });

    b.accessor("isPlaying", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (!n) return ev::fromBool(false);
        if (n->type() == scene::SceneNode::Type::Particles)
            return ev::fromBool(static_cast<scene::ParticleNode*>(n)->isPlaying());
        if (n->type() == scene::SceneNode::Type::Particles3D)
            return ev::fromBool(static_cast<scene::Particles3DNode*>(n)->isPlaying());
        return ev::fromBool(false);
    }, nullptr);

    b.def("play", 1, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n) return ev::undefined();
        if (n->type() == scene::SceneNode::Type::Particles) {
            static_cast<scene::ParticleNode*>(n)->play();
        } else if (n->type() == scene::SceneNode::Type::Particles3D) {
            static_cast<scene::Particles3DNode*>(n)->play();
        }
        return ev::undefined();
    });

    b.def("stop", 0, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n) return ev::undefined();
        if (n->type() == scene::SceneNode::Type::Particles)
            static_cast<scene::ParticleNode*>(n)->stop();
        else if (n->type() == scene::SceneNode::Type::Particles3D)
            static_cast<scene::Particles3DNode*>(n)->stop();
        return ev::undefined();
    });

    // Gaussian Splat
    b.accessor("splatCount", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (n && n->type() == scene::SceneNode::Type::GaussianSplat) {
            return ev::fromDouble(static_cast<double>(static_cast<scene::GaussianSplatNode*>(n)->splatCount()));
        }
        return ev::fromDouble(0.0);
    }, nullptr);

    b.def("savePly", 1, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n || n->type() != scene::SceneNode::Type::GaussianSplat) {
            return ev::throwError("savePly: node is not a GaussianSplat");
        }
        if (a.empty() || !ev::isString(a[0])) {
            return ev::throwError("savePly(path): path required");
        }
        auto* sn = static_cast<scene::GaussianSplatNode*>(n);
        if (sn->splatCount() == 0) {
            return ev::throwError("savePly: splat cloud is empty");
        }
        std::string path = ev::toUtf8(a[0]);
        bool ok = bromesh::saveSplatPLY(sn->cloud(), path);
        if (!ok) {
            return ev::throwError("savePly: failed to write file");
        }
        return ev::fromBool(true);
    });
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
