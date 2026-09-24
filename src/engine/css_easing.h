#pragma once

// CSS <easing-function> (CSS Easing Functions Level 2): the one timing-function
// type shared by CSS transitions, CSS animations and the Web Animations API.
//
//   linear | ease | ease-in | ease-out | ease-in-out | cubic-bezier(x1,y1,x2,y2)
//   step-start | step-end | steps(n [, jump-start|jump-end|jump-none|jump-both|
//                                      start|end])
//   linear(<number> [<percentage>{1,2}]#)
//
// It used to be a bare bromath::CubicEase, which cannot express steps() or
// linear(): both silently fell back to `ease`, and getTiming().easing then
// reported the fallback.

#include <bromath/curves.h>

#include <string>
#include <vector>

namespace bro::engine {

struct TimingFunction {
    enum class Kind { Cubic, Steps, Linear };
    enum class StepPosition { JumpStart, JumpEnd, JumpNone, JumpBoth };

    Kind kind = Kind::Cubic;
    bromath::CubicEase cubic{0.0f, 0.0f, 1.0f, 1.0f};  // Kind::Cubic
    int steps = 1;                                       // Kind::Steps
    StepPosition position = StepPosition::JumpEnd;       // Kind::Steps
    // Kind::Linear control points (input, output), input non-decreasing.
    std::vector<std::pair<float, float>> points;

    static TimingFunction linear() { return {}; }
    static TimingFunction ease() {
        TimingFunction f;
        f.cubic = {0.25f, 0.1f, 0.25f, 1.0f};
        return f;
    }
    static TimingFunction fromCubic(const bromath::CubicEase& c) {
        TimingFunction f;
        f.cubic = c;
        return f;
    }

    // Output progress for input progress `t`. Cubic curves and linear() may
    // leave [0,1] (overshoot); steps() never does. `beforeFlag` is the
    // Web Animations "before flag" for step functions at t == 0.
    float apply(float t, bool beforeFlag = false) const;

    bool isLinear() const {
        return kind == Kind::Cubic && cubic.p1x == cubic.p1y && cubic.p2x == cubic.p2y;
    }

    // Canonical serialization, as getComputedStyle / getTiming() report it:
    // the keyword when one matches, else the function (`steps(4)` for the
    // default jump-end position, as browsers print it).
    std::string toString() const;

    bool operator==(const TimingFunction& o) const;
    bool operator!=(const TimingFunction& o) const { return !(*this == o); }
};

// Parse an <easing-function>. On a string that is not one, returns false and
// leaves `out` alone. The Web Animations API throws a TypeError on that; CSS
// drops the declaration.
bool tryParseEasing(const std::string& text, TimingFunction& out);

// Parse, falling back to `ease` (the CSS initial value) for empty or invalid
// input. For CSS longhands, whose values the cascade has already validated.
TimingFunction parseTimingFunction(const std::string& text);

} // namespace bro::engine
