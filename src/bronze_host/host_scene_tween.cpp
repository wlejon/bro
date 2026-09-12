#if BRO_WITH_3D

#include "bronze_host/host_scene_internal.h"
#include "scene/scene_graph.h"
#include "scene/tween.h"
#include <cmath>

namespace bro::bronze_host {

HostClass g_tweenClass;

namespace {

static bool readTweenVec3(Value v, bromath::Vec3& out) {
    if (ev::isNumber(v)) {
        float f = static_cast<float>(ev::toDouble(v));
        out = {f, f, f};
        return true;
    }
    if (!ev::isObject(v)) return false;
    double x = 0, y = 0, z = 0;
    Value e0 = ev::getElement(v, 0);
    Value e1 = ev::getElement(v, 1);
    Value e2 = ev::getElement(v, 2);
    if (!ev::isUndefined(e0)) x = ev::toDouble(e0);
    if (!ev::isUndefined(e1)) y = ev::toDouble(e1);
    if (!ev::isUndefined(e2)) z = ev::toDouble(e2);
    out = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
    return true;
}

static bool readTweenQuat(Value v, bromath::Quat& out) {
    if (ev::isNumber(v)) {
        out = bromath::qfromEuler(0.0f, 0.0f, static_cast<float>(ev::toDouble(v)));
        return true;
    }
    if (ev::isObject(v)) {
        Value lenV = ev::getProperty(v, "length");
        if (ev::isNumber(lenV)) {
            double q[4] = {0, 0, 0, 1};
            for (uint32_t i = 0; i < 4; ++i) {
                Value elem = ev::getElement(v, i);
                if (!ev::isUndefined(elem)) q[i] = ev::toDouble(elem);
            }
            out = bromath::qnorm({static_cast<float>(q[0]), static_cast<float>(q[1]),
                                  static_cast<float>(q[2]), static_cast<float>(q[3])});
            return true;
        }
        Value axisVal = ev::getProperty(v, "axis");
        bromath::Vec3 axis;
        if (readTweenVec3(axisVal, axis)) {
            Value angVal = ev::getProperty(v, "angle");
            double angle = ev::isNumber(angVal) ? ev::toDouble(angVal) : 0.0;
            out = bromath::qaxisAngle(axis, static_cast<float>(angle));
            return true;
        }
    }
    return false;
}

static bool readTweenColor(Value v, bromath::Vec3& out) {
    if (ev::isString(v)) {
        float r = 1, g = 1, b = 1, a = 1;
        if (!parseColorValue(v, r, g, b, a)) return false;
        out = {r, g, b};
        return true;
    }
    return readTweenVec3(v, out);
}

static void parseTweenProps(Value props, uint32_t nodeId,
                            float duration, float delay, scene::Tween::Ease ease,
                            std::vector<scene::Tween::Anim>& out) {
    using Anim = scene::Tween::Anim;
    using Prop = scene::Tween::Prop;
    auto base = [&](Prop p) {
        Anim a;
        a.nodeId = nodeId;
        a.prop = p;
        a.duration = duration;
        a.delay = delay;
        a.ease = ease;
        return a;
    };

    Value v = ev::getProperty(props, "position");
    if (!ev::isUndefined(v)) {
        Anim a = base(Prop::Position);
        if (readTweenVec3(v, a.v3To)) out.push_back(std::move(a));
    }

    for (const char* key : {"rotation", "quaternion"}) {
        v = ev::getProperty(props, key);
        if (!ev::isUndefined(v)) {
            Anim a = base(Prop::Quaternion);
            if (readTweenQuat(v, a.qTo)) out.push_back(std::move(a));
        }
    }

    v = ev::getProperty(props, "scale");
    if (!ev::isUndefined(v)) {
        Anim a = base(Prop::Scale);
        if (readTweenVec3(v, a.v3To)) out.push_back(std::move(a));
    }

    v = ev::getProperty(props, "opacity");
    if (ev::isNumber(v)) {
        Anim a = base(Prop::Opacity);
        a.fTo = static_cast<float>(ev::toDouble(v));
        out.push_back(std::move(a));
    }

    v = ev::getProperty(props, "color");
    if (!ev::isUndefined(v)) {
        Anim a = base(Prop::Color);
        if (readTweenColor(v, a.v3To)) out.push_back(std::move(a));
    }
}

static bool s_tweenClassInstalled = false;

} // namespace

void ensureTweenClassInstalled() {
    if (s_tweenClassInstalled) return;
    s_tweenClassInstalled = true;

    g_tweenClass.install("Tween", 0, nullptr, [](ObjectBuilder& b) {
        b.accessor("isRunning", [](Value self_, std::span<const Value>) {
            auto* t = tweenOf(self_);
            return ev::fromBool(t && t->isRunning());
        }, nullptr);

        b.accessor("isPaused", [](Value self_, std::span<const Value>) {
            auto* t = tweenOf(self_);
            return ev::fromBool(t && t->isPaused());
        }, nullptr);

        b.accessor("isFinished", [](Value self_, std::span<const Value>) {
            auto* t = tweenOf(self_);
            return ev::fromBool(t && t->isFinished());
        }, nullptr);

        b.accessor("onFinished",
            [](Value, std::span<const Value>) { return ev::undefined(); },
            [](Value self_, std::span<const Value> a) {
                auto* t = tweenOf(self_);
                if (!t) return ev::undefined();
                if (!a.empty() && ev::isFunction(a[0])) {
                    auto fnRef = std::make_shared<ev::Persistent>(a[0]);
                    t->setOnFinished([fnRef]() {
                        if (fnRef && ev::isFunction(fnRef->get())) {
                            ev::call(fnRef->get(), ev::undefined(), {});
                        }
                    });
                } else {
                    t->setOnFinished(nullptr);
                }
                return ev::undefined();
            });

        b.def("to", 3, [](Value self_, std::span<const Value> a) {
            auto* t = tweenOf(self_);
            if (!t) return ev::throwTypeError("to: tween has been destroyed");
            if (a.size() < 3)
                return ev::throwTypeError("to(node, props, duration[, opts]) requires 3 arguments");

            uint32_t nodeId = 0;
            if (!ev::isNull(a[0]) && !ev::isUndefined(a[0])) {
                auto* n = sceneNodeOf(a[0]);
                if (!n) return ev::throwTypeError("to: first argument must be a SceneNode or null");
                nodeId = n->id();
            }
            if (!ev::isObject(a[1]))
                return ev::throwTypeError("to: props must be an object");

            float duration = static_cast<float>(ev::toDouble(a[2]));
            if (!(duration >= 0.0f)) duration = 0.0f;

            float delay = 0.0f;
            auto ease = scene::Tween::Ease::Linear;
            Value onUpdateVal = ev::undefined();
            if (a.size() > 3 && ev::isObject(a[3])) {
                Value easingVal = ev::getProperty(a[3], "easing");
                std::string easing = ev::isString(easingVal) ? ev::toUtf8(easingVal) : "linear";
                if (!scene::Tween::easeFromString(easing, ease))
                    return ev::throwTypeError("to: unknown easing '" + easing + "'");

                Value delayVal = ev::getProperty(a[3], "delay");
                if (ev::isNumber(delayVal)) delay = static_cast<float>(ev::toDouble(delayVal));
                onUpdateVal = ev::getProperty(a[3], "onUpdate");
            }

            std::vector<scene::Tween::Anim> anims;
            parseTweenProps(a[1], nodeId, duration, delay, ease, anims);

            if (ev::isFunction(onUpdateVal)) {
                scene::Tween::Anim anim;
                anim.prop = scene::Tween::Prop::Custom;
                anim.duration = duration;
                anim.delay = delay;
                anim.ease = ease;
                auto fnRef = std::make_shared<ev::Persistent>(onUpdateVal);
                anim.onUpdate = [fnRef](float val) {
                    if (fnRef && ev::isFunction(fnRef->get())) {
                        const Value arg = ev::fromDouble(val);
                        ev::call(fnRef->get(), ev::undefined(), std::span<const Value>(&arg, 1));
                    }
                };
                anims.push_back(std::move(anim));
            } else if (anims.empty()) {
                scene::Tween::Anim anim;
                anim.prop = scene::Tween::Prop::Custom;
                anim.duration = duration;
                anim.delay = delay;
                anim.ease = ease;
                anims.push_back(std::move(anim));
            }

            t->addAnims(std::move(anims));
            return self_;
        });

        b.def("parallel", 0, [](Value self_, std::span<const Value>) {
            if (auto* t = tweenOf(self_)) t->parallel();
            return self_;
        });

        b.def("call", 1, [](Value self_, std::span<const Value> a) {
            auto* t = tweenOf(self_);
            if (!t) return ev::throwTypeError("call: tween has been destroyed");
            if (a.empty() || !ev::isFunction(a[0]))
                return ev::throwTypeError("call(fn) requires a function");
            auto fnRef = std::make_shared<ev::Persistent>(a[0]);
            t->addCall([fnRef]() {
                if (fnRef && ev::isFunction(fnRef->get())) {
                    ev::call(fnRef->get(), ev::undefined(), {});
                }
            });
            return self_;
        });

        b.def("loop", 1, [](Value self_, std::span<const Value> a) {
            auto* t = tweenOf(self_);
            if (!t) return ev::throwTypeError("loop: tween has been destroyed");
            int n = -1;
            if (!a.empty() && ev::isNumber(a[0])) {
                double d = ev::toDouble(a[0]);
                if (std::isfinite(d)) n = static_cast<int>(d);
            }
            t->setLoops(n);
            return self_;
        });

        b.def("start", 0, [](Value self_, std::span<const Value>) {
            auto* t = tweenOf(self_);
            if (!t) return ev::throwTypeError("start: tween has been destroyed");
            t->start();
            return self_;
        });

        b.def("stop", 0, [](Value self_, std::span<const Value>) {
            if (auto* t = tweenOf(self_)) t->stop();
            return self_;
        });

        b.def("pause", 0, [](Value self_, std::span<const Value>) {
            if (auto* t = tweenOf(self_)) t->pause();
            return self_;
        });

        b.def("resume", 0, [](Value self_, std::span<const Value>) {
            if (auto* t = tweenOf(self_)) t->resume();
            return self_;
        });

        b.def("destroy", 0, [](Value self_, std::span<const Value>) {
            auto* c = tweenCellOf(self_);
            if (c && c->graph()) c->graph()->destroyTween(c->id);
            return ev::undefined();
        });
    });
}

Value wrapTween(scene::Tween* tween, scene::SceneGraph* graph) {
    if (!tween || !graph) return ev::null();
    auto tok = graph->livenessToken();
    if (!tok) return ev::null();
    ensureTweenClassInstalled();
    auto* cell = new HostTweenCell{kHostTweenTag, tok, tween->id()};
    return g_tweenClass.make(cell, [](void* p) {
        delete static_cast<HostTweenCell*>(p);
    });
}

} // namespace bro::bronze_host

#endif // BRO_WITH_3D
