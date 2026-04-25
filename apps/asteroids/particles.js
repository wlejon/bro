// particles.js — A.FX wrapper around lib/particles.js.
//
// Asteroids' callsites pass time and speed in milliseconds (life in ms,
// speed in px/ms). The lib speaks seconds. This shim does the conversion
// so game.js stays untouched.
var A = A || {};

A.FX = (function () {
    'use strict';

    const sys = Particles.createSystem({ wrap: true, cap: 600 });
    let lastW = 900, lastH = 800;

    function spawn(x, y, count, opts) {
        opts = opts || {};
        const baseSpeed = (opts.speed || 0.15) * 1000;     // px/ms -> px/s
        const baseLife  = (opts.life  || 600)  / 1000;     // ms    -> s
        const lifeVar   = (opts.lifeVar || 400) / 1000;
        const biasX = (opts.vx || 0) * 1000;
        const biasY = (opts.vy || 0) * 1000;
        const color = opts.color || '#ffffff';
        for (let i = 0; i < count; i++) {
            const ang = Math.random() * Math.PI * 2;
            const sp  = baseSpeed * (0.5 + Math.random());
            Particles.add(sys, {
                x: x, y: y,
                vx: Math.cos(ang) * sp + biasX,
                vy: Math.sin(ang) * sp + biasY,
                life:    baseLife + Math.random() * lifeVar,
                color:   color,
                size:    1,
                gravity: 0,
                drag:    1,
            });
        }
    }

    function update(dt) {
        Particles.step(sys, dt / 1000, lastW, lastH);
    }

    function draw(ctx, W, H) {
        if (W) lastW = W;
        if (H) lastH = H;
        // Asteroids draws particles as 2px squares, not pegbounce's circles.
        const list = sys.list;
        for (let i = 0; i < list.length; i++) {
            const p = list[i];
            ctx.globalAlpha = Math.max(0, p.life / p.maxLife);
            ctx.fillStyle = p.color;
            ctx.fillRect(p.x - 1, p.y - 1, 2, 2);
        }
        ctx.globalAlpha = 1;
    }

    function clear() { Particles.clear(sys); }

    return { spawn, update, draw, clear };
})();
