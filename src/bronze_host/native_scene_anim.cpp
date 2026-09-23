// native_scene_anim.cpp — Tween native implementations and animation namespace registration.

#if BRO_WITH_3D

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
#include "natives/animation/native_animation_decl.h"
#include "scene/tween.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

bool registerNatives_animation(std::string* error);
bool registerClipPlayerNatives(std::string* error);

namespace {


static bool readTweenVec3(Value v, bromath::Vec3& out) {
    if (ev::isNumber(v)) {
        float f = static_cast<float>(ev::toDouble(v));
        out = {f, f, f};
        return true;
    }
    if (!ev::isObject(v)) return false;
    const Rooted arr(v);
    // Each element converts before the next read can move it.
    double xyz[3] = {0, 0, 0};
    for (uint32_t i = 0; i < 3; ++i) {
        Value e = ev::getElement(arr, i);
        if (!ev::isUndefined(e)) xyz[i] = ev::toDouble(e);
    }
    out = {static_cast<float>(xyz[0]), static_cast<float>(xyz[1]), static_cast<float>(xyz[2])};
    return true;
}

static bool readTweenQuat(Value vIn, bromath::Quat& out) {
    if (ev::isNumber(vIn)) {
        out = bromath::qfromEuler(0.0f, 0.0f, static_cast<float>(ev::toDouble(vIn)));
        return true;
    }
    if (ev::isObject(vIn)) {
        const Rooted v(vIn);
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

static bool readTweenColor(Value vIn, bromath::Vec3& out) {
    const Rooted v(vIn);
    float r = 1, g = 1, b = 1, a = 1;
    if (parseColorValue(v, r, g, b, a)) {
        out = {r, g, b};
        return true;
    }
    return readTweenVec3(v, out);
}

static void parseTweenProps(Value propsIn, uint32_t nodeId,
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

    const Rooted props(propsIn);
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

}  // namespace

}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;

// =============================================================================
// Tween lifecycle & base methods
// =============================================================================

// A native declared to return `__bro_native.animation.Tween` hands its
// pointer to bronze_native_wrap, which mints a NEW handle that OWNS it and
// runs Tween_dtor on it when that handle dies (runtime/native_handle.h). So
// a chainable method must not answer `self`: two handles over one cell is a
// double delete at the sweep, which is exactly the heap corruption every
// test that called start() died of at exit. Each chained call gets its own
// cell instead; a cell is a liveness token plus the tween id, so the copy
// resolves to the same scene::Tween and costs nothing.
static void* chained(HostTweenCell* c) {
    return c ? new HostTweenCell(*c) : nullptr;
}

void bro_animation_Tween_dtor(void* self) {
    delete tweenCellOf(self);
}

void* bro_animation_Tween_ctor(void) {
    return new HostTweenCell();
}

void* bro_animation_Tween_call(void* self, uint64_t callback) {
    auto* c = tweenCellOf(self);
    if (c && c->tween()) {
        auto fnRef = std::make_shared<ev::Persistent>(ev::fromBits(callback));
        c->tween()->addCall([fnRef]() {
            if (fnRef && ev::isFunction(fnRef->get())) {
                ev::call(fnRef->get(), ev::undefined(), {});
            }
        });
    }
    return chained(c);
}

void* bro_animation_Tween_loop(void* self, bool count_given, int32_t count) {
    auto* c = tweenCellOf(self);
    if (c && c->tween()) {
        c->tween()->setLoops(count_given ? count : -1);
    }
    return chained(c);
}

void* bro_animation_Tween_start(void* self) {
    auto* c = tweenCellOf(self);
    if (!c || !c->tween()) {
        ev::throwError("start() on a destroyed tween");
        return nullptr;
    }
    c->tween()->start();
    return chained(c);
}

void* bro_animation_Tween_stop(void* self) {
    auto* c = tweenCellOf(self);
    if (c && c->tween()) c->tween()->stop();
    return chained(c);
}

void* bro_animation_Tween_pause(void* self) {
    auto* c = tweenCellOf(self);
    if (c && c->tween()) c->tween()->pause();
    return chained(c);
}

void* bro_animation_Tween_resume(void* self) {
    auto* c = tweenCellOf(self);
    if (c && c->tween()) c->tween()->resume();
    return chained(c);
}

void bro_animation_Tween_destroy(void* self) {
    auto* c = tweenCellOf(self);
    if (c && c->graph()) {
        c->graph()->destroyTween(c->id);
    }
}

// =============================================================================
// Tween extended methods
// =============================================================================

void* bro_animation_Tween_to(void* self, int32_t targetId, uint64_t propsBits, double duration, uint64_t optsBits, uint64_t onUpdateBits) {
    auto* c = tweenCellOf(self);
    if (!c || !c->tween()) {
        ev::throwError("to() on a destroyed tween");
        return nullptr;
    }
    auto* t = c->tween();

    uint32_t nodeId = targetId > 0 ? static_cast<uint32_t>(targetId) : 0;

    // All three are read after the option reads below allocate.
    const Rooted props(ev::fromBits(propsBits));
    const Rooted opts(ev::fromBits(optsBits));
    const Rooted onUpdate(ev::fromBits(onUpdateBits));
    float dur = static_cast<float>(duration);
    if (!(dur >= 0.0f)) dur = 0.0f;

    float delay = 0.0f;
    auto ease = scene::Tween::Ease::Linear;
    if (ev::isObject(opts)) {
        Value easingVal = ev::getProperty(opts, "easing");
        if (ev::isString(easingVal)) {
            std::string easingStr = ev::toUtf8(easingVal);
            if (!scene::Tween::easeFromString(easingStr, ease)) {
                ev::throwTypeError("to: unknown easing '" + easingStr + "'");
                return nullptr;
            }
        }
        Value delayVal = ev::getProperty(opts, "delay");
        if (ev::isNumber(delayVal)) delay = static_cast<float>(ev::toDouble(delayVal));
    }

    std::vector<scene::Tween::Anim> anims;
    if (ev::isObject(props)) {
        parseTweenProps(props, nodeId, dur, delay, ease, anims);
    }

    if (ev::isFunction(onUpdate)) {
        scene::Tween::Anim anim;
        anim.prop = scene::Tween::Prop::Custom;
        anim.duration = dur;
        anim.delay = delay;
        anim.ease = ease;
        auto fnRef = std::make_shared<ev::Persistent>(onUpdate.get());
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
        anim.duration = dur;
        anim.delay = delay;
        anim.ease = ease;
        anims.push_back(std::move(anim));
    }

    t->addAnims(std::move(anims));
    return self;
}

void* bro_animation_Tween_parallel(void* self) {
    auto* c = tweenCellOf(self);
    if (c && c->tween()) c->tween()->parallel();
    return self;
}

bool bro_animation_Tween_isRunning(void* self) {
    auto* c = tweenCellOf(self);
    return c && c->tween() && c->tween()->isRunning();
}

bool bro_animation_Tween_isPaused(void* self) {
    auto* c = tweenCellOf(self);
    return c && c->tween() && c->tween()->isPaused();
}

bool bro_animation_Tween_isFinished(void* self) {
    auto* c = tweenCellOf(self);
    return c && c->tween() && c->tween()->isFinished();
}

void bro_animation_Tween_setOnFinished(void* self, uint64_t cbBits) {
    auto* c = tweenCellOf(self);
    if (!c || !c->tween()) return;
    Value cb = ev::fromBits(cbBits);
    if (ev::isFunction(cb)) {
        auto fnRef = std::make_shared<ev::Persistent>(cb);
        c->tween()->setOnFinished([fnRef]() {
            if (fnRef && ev::isFunction(fnRef->get())) {
                ev::call(fnRef->get(), ev::undefined(), {});
            }
        });
    } else {
        c->tween()->setOnFinished(nullptr);
    }
}

}  // extern "C"

namespace bro::bronze_host {

bool registerAnimationNatives(std::string* error) {
    if (!registerNatives_animation(error)) return false;

    using namespace natives;
    return fn("__bro_native.animation.Tween_to", (void*)&bro_animation_Tween_to, "void", {"__bro_native.animation.Tween", "i32", "dynamic", "f64", "dynamic", "dynamic"}, error) &&
           fn("__bro_native.animation.Tween_parallel", (void*)&bro_animation_Tween_parallel, "void", {"__bro_native.animation.Tween"}, error) &&
           fn("__bro_native.animation.Tween_isRunning", (void*)&bro_animation_Tween_isRunning, "bool", {"__bro_native.animation.Tween"}, error) &&
           fn("__bro_native.animation.Tween_isPaused", (void*)&bro_animation_Tween_isPaused, "bool", {"__bro_native.animation.Tween"}, error) &&
           fn("__bro_native.animation.Tween_isFinished", (void*)&bro_animation_Tween_isFinished, "bool", {"__bro_native.animation.Tween"}, error) &&
           fn("__bro_native.animation.Tween_setOnFinished", (void*)&bro_animation_Tween_setOnFinished, "void", {"__bro_native.animation.Tween", "dynamic"}, error) &&
           registerClipPlayerNatives(error);
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
