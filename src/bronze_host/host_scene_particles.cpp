#if BRO_WITH_3D

#include "bronze_host/host_scene_internal.h"
#include "scene/particles3d_node.h"
#include "scene/particle_node.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "util/asset_path.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

static double numAtProp(Value obj, const char* key, double defVal) {
    if (!ev::isObject(obj)) return defVal;
    Value v = ev::getProperty(obj, key);
    return ev::isNumber(v) ? ev::toDouble(v) : defVal;
}

static bool parseRange(Value obj, const char* key, float& lo, float& hi) {
    if (!ev::isObject(obj)) return false;
    Value v = ev::getProperty(obj, key);
    if (ev::isNumber(v)) {
        float f = static_cast<float>(ev::toDouble(v));
        lo = f; hi = f;
        return true;
    }
    if (ev::isObject(v)) {
        Value minV = ev::getProperty(v, "min");
        Value maxV = ev::getProperty(v, "max");
        if (ev::isUndefined(minV)) minV = ev::getProperty(v, "lo");
        if (ev::isUndefined(maxV)) maxV = ev::getProperty(v, "hi");
        if (ev::isNumber(minV)) lo = static_cast<float>(ev::toDouble(minV));
        if (ev::isNumber(maxV)) hi = static_cast<float>(ev::toDouble(maxV));
        return true;
    }
    return false;
}

static scene::Particles3DNode::EmitterShape particleShapeFromString(const std::string& s) {
    using S = scene::Particles3DNode::EmitterShape;
    if (s == "sphere")     return S::Sphere;
    if (s == "hemisphere") return S::Hemisphere;
    if (s == "box")        return S::Box;
    if (s == "cone")       return S::Cone;
    return S::Point;
}

static void applyParticle3DOpts(scene::Particles3DNode* node, Value opts) {
    if (!ev::isObject(opts)) return;

    Value nameVal = ev::getProperty(opts, "name");
    if (ev::isString(nameVal)) node->setName(ev::toUtf8(nameVal));

    Value mpVal = ev::getProperty(opts, "maxParticles");
    if (ev::isNumber(mpVal)) {
        node->setMaxParticles(static_cast<int>(ev::toDouble(mpVal)));
    }

    Value seedVal = ev::getProperty(opts, "seed");
    if (ev::isNumber(seedVal)) {
        node->setSeed(static_cast<uint64_t>(ev::toDouble(seedVal)));
    }

    Value texVal = ev::getProperty(opts, "texture");
    if (ev::isString(texVal)) {
        node->setTexturePath(util::resolveAssetPath(ev::toUtf8(texVal)));
    }

    Value sheetVal = ev::getProperty(opts, "sheet");
    if (ev::isObject(sheetVal)) {
        node->setSheet(
            static_cast<int>(numAtProp(sheetVal, "cols", 1)),
            static_cast<int>(numAtProp(sheetVal, "rows", 1)),
            static_cast<int>(numAtProp(sheetVal, "frames", 0)));
    }

    Value blendVal = ev::getProperty(opts, "blend");
    if (ev::isString(blendVal)) {
        std::string s = ev::toUtf8(blendVal);
        node->setBlend(s == "additive" ? scene::Particles3DNode::Blend::Additive
                                       : scene::Particles3DNode::Blend::Normal);
    }

    Value shapeVal = ev::getProperty(opts, "shape");
    if (ev::isString(shapeVal)) {
        node->setShape(particleShapeFromString(ev::toUtf8(shapeVal)));
    } else if (ev::isObject(shapeVal)) {
        Value typeVal = ev::getProperty(shapeVal, "type");
        std::string t = ev::isString(typeVal) ? ev::toUtf8(typeVal) : "point";
        node->setShape(particleShapeFromString(t));
        Value rVal = ev::getProperty(shapeVal, "radius");
        if (ev::isNumber(rVal)) node->setShapeRadius(static_cast<float>(ev::toDouble(rVal)));
        Value aVal = ev::getProperty(shapeVal, "angle");
        if (ev::isNumber(aVal)) node->setConeAngle(static_cast<float>(ev::toDouble(aVal)));
        bromath::Vec3 he;
        Value extVal = ev::getProperty(shapeVal, "extents");
        if (readVec3FromValue(extVal, he)) node->setShapeExtents(he);
    }

    Value spaceVal = ev::getProperty(opts, "space");
    if (ev::isString(spaceVal)) {
        node->setSpace(ev::toUtf8(spaceVal) == "local"
                           ? scene::Particles3DNode::SimSpace::Local
                           : scene::Particles3DNode::SimSpace::World);
    }

    Value rateVal = ev::getProperty(opts, "rate");
    if (ev::isNumber(rateVal)) node->setRate(static_cast<float>(ev::toDouble(rateVal)));

    float lo = 0.5f, hi = 1.0f;
    if (parseRange(opts, "lifetime", lo, hi)) {
        node->setLifetime(lo, hi);
    }

    Value velVal = ev::getProperty(opts, "velocity");
    if (ev::isObject(velVal)) {
        bromath::Vec3 dir{0.0f, 1.0f, 0.0f};
        Value dirVal = ev::getProperty(velVal, "direction");
        readVec3FromValue(dirVal, dir);
        float spread = static_cast<float>(numAtProp(velVal, "spread", 0));
        node->setDirection(dir, spread);
        float speed = static_cast<float>(numAtProp(velVal, "speed", 1));
        float speedSpread = static_cast<float>(numAtProp(velVal, "speedSpread", 0));
        node->setSpeed(speed, speedSpread);
    }

    bromath::Vec3 g;
    Value gravVal = ev::getProperty(opts, "gravity");
    if (readVec3FromValue(gravVal, g)) node->setGravity(g);

    Value sizeVal = ev::getProperty(opts, "size");
    if (ev::isObject(sizeVal)) {
        node->setSize(
            static_cast<float>(numAtProp(sizeVal, "start", 0.1)),
            static_cast<float>(numAtProp(sizeVal, "end", 0)));
    } else if (ev::isNumber(sizeVal)) {
        float v = static_cast<float>(ev::toDouble(sizeVal));
        node->setSize(v, v);
    }

    Value colorVal = ev::getProperty(opts, "color");
    Value lenVal = ev::isObject(colorVal) ? ev::getProperty(colorVal, "length") : ev::undefined();
    if (ev::isNumber(lenVal)) {
        int len = static_cast<int>(ev::toDouble(lenVal));
        std::vector<std::pair<float, bromath::Color>> stops;
        for (int i = 0; i < len; ++i) {
            Value e = ev::getElement(colorVal, i);
            float t = (len > 1) ? static_cast<float>(i) / static_cast<float>(len - 1) : 0.0f;
            float r = 1, g_ = 1, b = 1, a = 1;
            if (ev::isString(e)) {
                parseColorValue(e, r, g_, b, a);
            } else if (ev::isObject(e)) {
                t = static_cast<float>(numAtProp(e, "t", t));
                Value cv = ev::getProperty(e, "color");
                parseColorValue(cv, r, g_, b, a);
            }
            stops.emplace_back(t, bromath::Color{r, g_, b, a});
        }
        std::stable_sort(stops.begin(), stops.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        node->setColorStops(std::move(stops));
    } else if (ev::isObject(colorVal)) {
        Value cs = ev::getProperty(colorVal, "start");
        Value ce = ev::getProperty(colorVal, "end");
        float sr = 1, sg = 1, sb = 1, sa = 1;
        float er = 1, eg = 1, eb = 1, ea = 0;
        parseColorValue(cs, sr, sg, sb, sa);
        if (!parseColorValue(ce, er, eg, eb, ea)) {
            er = sr; eg = sg; eb = sb; ea = 0.0f;
        }
        node->setColors(bromath::Color{sr, sg, sb, sa}, bromath::Color{er, eg, eb, ea});
    } else if (ev::isString(colorVal)) {
        float r = 1, g_ = 1, b = 1, a = 1;
        parseColorValue(colorVal, r, g_, b, a);
        node->setColors(bromath::Color{r, g_, b, a}, bromath::Color{r, g_, b, 0.0f});
    }

    Value rotVal = ev::getProperty(opts, "rotation");
    if (ev::isObject(rotVal)) {
        node->setRotation(
            static_cast<float>(numAtProp(rotVal, "start", 0)),
            static_cast<float>(numAtProp(rotVal, "spinSpeed", 0)),
            static_cast<float>(numAtProp(rotVal, "spinSpread", 0)));
    }

    Value dragVal = ev::getProperty(opts, "drag");
    if (ev::isNumber(dragVal)) node->setDrag(static_cast<float>(ev::toDouble(dragVal)));

    Value softVal = ev::getProperty(opts, "softness");
    if (ev::isNumber(softVal)) node->setSoftness(static_cast<float>(ev::toDouble(softVal)));

    Value durVal = ev::getProperty(opts, "duration");
    Value loopVal = ev::getProperty(opts, "loop");
    if (ev::isNumber(durVal)) {
        float dur = static_cast<float>(ev::toDouble(durVal));
        bool loop = ev::isUndefined(loopVal) ? false : ev::toBool(loopVal);
        node->setDuration(dur, loop);
    } else if (!ev::isUndefined(loopVal)) {
        node->setDuration(node->duration(), ev::toBool(loopVal));
    }

    Value finVal = ev::getProperty(opts, "onFinished");
    if (ev::isFunction(finVal)) {
        auto fnRef = std::make_shared<ev::Persistent>(finVal);
        node->setOnFinished([fnRef]() {
            if (fnRef && ev::isFunction(fnRef->get())) {
                ev::call(fnRef->get(), ev::undefined(), {});
            }
        });
    }

    Value posVal = ev::getProperty(opts, "position");
    bromath::Vec3 pos;
    if (readVec3FromValue(posVal, pos)) {
        node->setPosition(pos.x, pos.y, pos.z);
    } else {
        double x = numAtProp(opts, "x", 0);
        double y = numAtProp(opts, "y", 0);
        double z = numAtProp(opts, "z", 0);
        if (x != 0 || y != 0 || z != 0) node->setPosition((float)x, (float)y, (float)z);
    }

    Value burstVal = ev::getProperty(opts, "burst");
    if (ev::isNumber(burstVal)) {
        node->burst(static_cast<int>(ev::toDouble(burstVal)));
    }

    Value apVal = ev::getProperty(opts, "autoplay");
    bool autoplay = ev::isUndefined(apVal) ? true : ev::toBool(apVal);
    if (!autoplay) node->stop();
}

} // namespace

void installSceneParticles(ObjectBuilder& bNode, ObjectBuilder& bGraph) {
    bGraph.def("createParticles3D", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::undefined();
        auto* node = g->createParticles3D();
        g->root()->addChild(node);
        if (!a.empty() && ev::isObject(a[0])) {
            applyParticle3DOpts(node, a[0]);
        }
        return wrapSceneNode(node, g);
    });

    bNode.accessor("particleCount", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (!n) return ev::fromDouble(0.0);
        if (n->type() == scene::SceneNode::Type::Particles)
            return ev::fromDouble(static_cast<scene::ParticleNode*>(n)->liveCount());
        if (n->type() == scene::SceneNode::Type::Particles3D)
            return ev::fromDouble(static_cast<scene::Particles3DNode*>(n)->liveCount());
        return ev::fromDouble(0.0);
    }, nullptr);

    bNode.accessor("liveCount", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (!n) return ev::fromDouble(0.0);
        if (n->type() == scene::SceneNode::Type::Particles3D)
            return ev::fromDouble(static_cast<scene::Particles3DNode*>(n)->liveCount());
        if (n->type() == scene::SceneNode::Type::Particles)
            return ev::fromDouble(static_cast<scene::ParticleNode*>(n)->liveCount());
        return ev::fromDouble(0.0);
    }, nullptr);

    bNode.accessor("rate",
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

    bNode.accessor("softness",
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

    bNode.accessor("onFinished",
        [](Value self_, std::span<const Value>) { return ev::undefined(); },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Particles3D && !a.empty() && ev::isFunction(a[0])) {
                auto fnRef = std::make_shared<ev::Persistent>(a[0]);
                static_cast<scene::Particles3DNode*>(n)->setOnFinished([fnRef]() {
                    if (fnRef && ev::isFunction(fnRef->get())) {
                        ev::call(fnRef->get(), ev::undefined(), {});
                    }
                });
            }
            return ev::undefined();
        });

    bNode.def("burst", 1, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n || a.empty() || !ev::isNumber(a[0])) return ev::undefined();
        int count = static_cast<int>(ev::toDouble(a[0]));
        if (n->type() == scene::SceneNode::Type::Particles)
            static_cast<scene::ParticleNode*>(n)->burst(count);
        else if (n->type() == scene::SceneNode::Type::Particles3D)
            static_cast<scene::Particles3DNode*>(n)->burst(count);
        return ev::undefined();
    });

    bNode.def("clear", 0, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n) return ev::undefined();
        if (n->type() == scene::SceneNode::Type::Particles)
            static_cast<scene::ParticleNode*>(n)->clear();
        else if (n->type() == scene::SceneNode::Type::Particles3D)
            static_cast<scene::Particles3DNode*>(n)->clear();
        return ev::undefined();
    });
}

} // namespace bro::bronze_host

#endif // BRO_WITH_3D
