// math.js — small math + RNG helpers used everywhere in apps/.
//
// Exposes the global `MathX` (Math is taken). All functions are pure.
//
// Usage:
//   <script src="../lib/math.js"></script>
//   const a = MathX.clamp(x, 0, 1);
//   const v = MathX.vecFromAngle(theta, speed);   // {x, y}
//   const i = MathX.randInt(0, 9);                 // inclusive both ends
//   const c = MathX.randPick(['r','g','b']);
//
// Design notes:
//   - randRange returns [lo, hi) (exclusive high) — matches Math.random scaling.
//   - randInt returns [lo, hi] inclusive — matches typical "pick N" usage.
//   - angleNorm wraps to [-PI, PI].
//   - lerpAngle takes the short way around.

(function (global) {
    'use strict';

    const PI = Math.PI;
    const TAU = PI * 2;

    function clamp(v, lo, hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    function lerp(a, b, t) {
        return a + (b - a) * t;
    }

    function angleNorm(a) {
        // Wrap to [-PI, PI].
        a = a % TAU;
        if (a > PI) a -= TAU;
        else if (a < -PI) a += TAU;
        return a;
    }

    function lerpAngle(a, b, t) {
        return a + angleNorm(b - a) * t;
    }

    function randRange(lo, hi) {
        return lo + Math.random() * (hi - lo);
    }

    function randInt(lo, hi) {
        // Inclusive both ends.
        return Math.floor(lo + Math.random() * (hi - lo + 1));
    }

    function randPick(arr) {
        return arr[Math.floor(Math.random() * arr.length)];
    }

    function vecFromAngle(a, mag) {
        if (mag === undefined) mag = 1;
        return { x: Math.cos(a) * mag, y: Math.sin(a) * mag };
    }

    function dist2(ax, ay, bx, by) {
        const dx = bx - ax, dy = by - ay;
        return dx * dx + dy * dy;
    }

    global.MathX = {
        clamp, lerp, lerpAngle, angleNorm,
        randRange, randInt, randPick,
        vecFromAngle, dist2,
        PI, TAU,
    };
})(typeof globalThis !== 'undefined' ? globalThis : this);
