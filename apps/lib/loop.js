// loop.js — requestAnimationFrame game loop with clamped dt.
//
// Usage:
//   <script src="../lib/loop.js"></script>
//   const loop = GameLoop.create({ tick: (dt) => update(dt), draw: () => render() });
//   loop.start();
//   // loop.pause(); loop.resume(); loop.stop();
//
// Extracted from the dt-clamped rAF pattern used in every 2D arcade app
// (blockfall, asteroids, breakout, invaders, …). dt is milliseconds.

(function (global) {
    'use strict';

    // Frames longer than this are likely tab-switch / breakpoint stalls.
    // Clamp so physics/animations don't explode across the gap.
    const MAX_DT_MS = 100;

    function create(opts) {
        opts = opts || {};
        const tick   = opts.tick   || function () {};
        const draw   = opts.draw   || function () {};
        const maxDt  = opts.maxDt  != null ? opts.maxDt : MAX_DT_MS;

        let running = false;
        let paused  = false;
        let lastT   = 0;
        let rafId   = 0;

        function frame(t) {
            if (!running) return;
            rafId = requestAnimationFrame(frame);
            const dt = Math.min(maxDt, t - lastT);
            lastT = t;
            if (!paused) tick(dt);
            draw(dt);
        }

        return {
            start() {
                if (running) return;
                running = true;
                paused  = false;
                lastT   = performance.now();
                rafId   = requestAnimationFrame(frame);
            },
            stop() {
                running = false;
                if (rafId) cancelAnimationFrame(rafId);
                rafId = 0;
            },
            pause()  { paused = true;  },
            resume() { paused = false; lastT = performance.now(); },
            isPaused()  { return paused; },
            isRunning() { return running; },
        };
    }

    global.GameLoop = { create };
})(typeof window !== 'undefined' ? window : globalThis);
