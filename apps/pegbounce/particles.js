// particles.js — lightweight particle system for peg hits, level clears.
// Each particle is a simple integrator: position, velocity, life, colour.

'use strict';
(function (global) {

    function createSystem() {
        return { list: [], cap: 800 };
    }

    function add(sys, opts) {
        if (sys.list.length >= sys.cap) return;
        sys.list.push({
            x: opts.x, y: opts.y,
            vx: opts.vx || 0, vy: opts.vy || 0,
            life: opts.life || 0.6,
            maxLife: opts.life || 0.6,
            color: opts.color || '#fff',
            size: opts.size || 2,
            gravity: opts.gravity != null ? opts.gravity : 0.0,
            drag: opts.drag != null ? opts.drag : 0.98,
        });
    }

    function burst(sys, x, y, color, count, speed) {
        for (let i = 0; i < count; i++) {
            const ang = Math.random() * Math.PI * 2;
            const sp  = (0.4 + Math.random() * 0.6) * (speed || 180);
            add(sys, {
                x, y,
                vx: Math.cos(ang) * sp,
                vy: Math.sin(ang) * sp,
                life: 0.5 + Math.random() * 0.3,
                color,
                size: 2 + Math.random() * 2,
                gravity: 360,
                drag: 0.97,
            });
        }
    }

    function step(sys, dtSec) {
        const out = [];
        for (const p of sys.list) {
            p.life -= dtSec;
            if (p.life <= 0) continue;
            p.vy += p.gravity * dtSec;
            p.vx *= p.drag;
            p.vy *= p.drag;
            p.x += p.vx * dtSec;
            p.y += p.vy * dtSec;
            out.push(p);
        }
        sys.list = out;
    }

    function draw(sys, ctx) {
        for (const p of sys.list) {
            const a = Math.max(0, p.life / p.maxLife);
            ctx.globalAlpha = a;
            ctx.fillStyle = p.color;
            ctx.beginPath();
            ctx.arc(p.x, p.y, p.size, 0, Math.PI * 2);
            ctx.fill();
        }
        ctx.globalAlpha = 1;
    }

    function clear(sys) { sys.list.length = 0; }

    global.Particles = { createSystem, add, burst, step, draw, clear };

})(typeof window !== 'undefined' ? window : globalThis);
