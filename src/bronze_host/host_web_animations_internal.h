#pragma once

// Shared between the Animation bindings (host_web_animations.cpp) and the
// timeline / KeyframeEffect bindings (host_web_animations_effect.cpp).

#include "bronze_host/host_internal.h"

#include <cstdint>
#include <string>

namespace bro::engine { struct WebAnimation; }

namespace bro::bronze_host {

// The native state behind one JS Animation (or CSSAnimation) object.
struct AnimationState {
    uint64_t id = 0;
    std::string name;
    ev::Persistent self;
    ev::Persistent onfinish;
    ev::Persistent oncancel;
    ev::Persistent onremove;
    ev::Persistent finishedPromise;
    // `ready`: minted on first read and already resolved — this model has no
    // pending play/pause tasks (`pending` is always false), so an animation is
    // ready the moment it exists.
    ev::Persistent readyPromise;
    // `effect`: the KeyframeEffect, minted on first read and kept so
    // `anim.effect === anim.effect`.
    ev::Persistent effect;
    bool promiseSettled = false;
    bool finishDelivered = false;
};

// Read the EffectTiming members of `obj` (element.animate() options,
// effect.updateTiming()) into `a`, recording which members were present in
// `setMask` (engine::WebAnimTimingField bits). False, with `error` set, on a
// value the spec answers with a TypeError (a negative or NaN duration, an
// unparseable easing, ...). `id` receives options.id when present.
bool parseEffectTiming(Value obj, engine::WebAnimation& a, uint32_t& setMask,
                       std::string& error, std::string* id);

void installTimelineAndEffectClasses();
Value documentTimelineValue();
// The KeyframeEffect for an animation, minted once.
Value effectValueFor(AnimationState* st);

} // namespace bro::bronze_host
