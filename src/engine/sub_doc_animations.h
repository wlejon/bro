#pragma once

// A sub-document's CSS transitions, CSS animations and Web Animations (an
// <iframe>'s, a bro.window.open() window's). The app document's are the
// engine's own, ticked by the layout thread's pass; a sub-document is styled
// and laid out on the main thread (recordSubDoc), which ticks these the same
// way: the resolve starts and samples them at the scaled clock (attach), the
// tick after it advances them and queues their events, and what is animating
// or just finished is marked for the next pass once the pass has cleared the
// document's dirty flag.

#include "engine/css_transitions.h"
#include "engine/web_animations.h"

#include <functional>
#include <vector>

namespace bro::dom { class Document; }

namespace bro::engine {

struct SubDocAnimations {
    TransitionManager transitions;
    AnimationManager animations;
    WebAnimationManager web;

    SubDocAnimations() {
        transitions.setWebAnimations(&web);
        animations.setWebAnimations(&web);
    }
    SubDocAnimations(const SubDocAnimations&) = delete;
    SubDocAnimations& operator=(const SubDocAnimations&) = delete;

    // Hand `doc` the managers, and the clock each style resolve of it starts
    // and samples them at (the engine's scaled clock: Document::setAnimationClock).
    void attach(dom::Document* doc, std::function<double()> clock);
    // After a resolve of `doc`: advance them to `nowMs`; true while one runs.
    bool tick(dom::Document* doc, double nowMs);
    // After the pass cleared the document's dirty flag: mark the elements
    // animating (their next value) and those whose animation just finished
    // (their settled value), so the next pass runs.
    void markForNextPass();
    // Its document is going: every record and its events.
    void clear();
};

}  // namespace bro::engine
