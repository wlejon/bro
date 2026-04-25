// fx.js — screen-level effects shared across games: shake + toast text.
//
// Two pieces, both ms-based:
//   • Screen shake — set a duration and magnitude; offset() returns a
//     random {x,y} that ramps down linearly over the duration.
//   • Toast — show a string in a DOM element for a duration, then hide.
//     Multiple toasts can coexist if they target different selectors.
//
// Usage:
//   FX.shake(300, 8);                     // 300ms, 8px peak
//   const off = FX.shakeOffset();         // {x,y} drift; {0,0} when idle
//   FX.toast('TETRIS');                   // default selector '#action-text'
//   FX.toast('LEVEL 4', { selector: '#cascade-text', duration: 700 });
//   FX.tick(dtMs);                        // call every frame
//   FX.reset();                           // clear shake + all toasts

(function (global) {
    'use strict';

    let shakeTimer = 0, shakeDur = 1, shakeMag = 0;

    // Map of selector -> { timer, el }.
    const toasts = {};

    function shake(duration, magnitude) {
        shakeTimer = duration || 0;
        shakeDur   = duration || 1;
        shakeMag   = magnitude || 0;
    }

    function shakeOffset() {
        if (shakeTimer <= 0) return { x: 0, y: 0 };
        const intensity = (shakeTimer / shakeDur) * shakeMag;
        return {
            x: (Math.random() - 0.5) * intensity,
            y: (Math.random() - 0.5) * intensity,
        };
    }

    function toast(text, opts) {
        opts = opts || {};
        const selector = opts.selector || '#action-text';
        const duration = opts.duration || 800;
        let entry = toasts[selector];
        if (!entry) entry = toasts[selector] = { el: null, timer: 0 };
        if (!entry.el) entry.el = document.querySelector(selector);
        if (entry.el) {
            entry.el.textContent = text;
            entry.el.style.display = 'block';
        }
        entry.timer = duration;
    }

    function tick(dt) {
        if (shakeTimer > 0) shakeTimer -= dt;
        for (const sel in toasts) {
            const e = toasts[sel];
            if (e.timer <= 0) continue;
            e.timer -= dt;
            if (e.timer <= 0 && e.el) e.el.style.display = 'none';
        }
    }

    function reset() {
        shakeTimer = 0;
        for (const sel in toasts) {
            const e = toasts[sel];
            e.timer = 0;
            if (e.el) e.el.style.display = 'none';
        }
    }

    global.FX = { shake, shakeOffset, toast, tick, reset };
})(typeof window !== 'undefined' ? window : globalThis);
