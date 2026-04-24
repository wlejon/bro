// particles.js — Simple cascade burst particles.
'use strict';
var G = G || {};

G.Particles = (function () {
    var particles = [];

    function burst(x, y, color, count) {
        count = count || 8;
        for (var i = 0; i < count; i++) {
            var ang = Math.random() * Math.PI * 2;
            var spd = 60 + Math.random() * 140;
            particles.push({
                x: x, y: y,
                vx: Math.cos(ang) * spd,
                vy: Math.sin(ang) * spd,
                life: 600 + Math.random() * 400,
                age: 0,
                color: color,
                size: 2 + Math.random() * 3,
            });
        }
    }

    function update(dt) {
        var kept = [];
        for (var i = 0; i < particles.length; i++) {
            var p = particles[i];
            p.age += dt;
            if (p.age >= p.life) continue;
            p.x += p.vx * dt / 1000;
            p.y += p.vy * dt / 1000;
            p.vy += 220 * dt / 1000; // gravity
            kept.push(p);
        }
        particles = kept;
    }

    function draw(ctx) {
        for (var i = 0; i < particles.length; i++) {
            var p = particles[i];
            var a = 1 - (p.age / p.life);
            ctx.fillStyle = p.color;
            ctx.globalAlpha = Math.max(0, a);
            ctx.fillRect(p.x - p.size / 2, p.y - p.size / 2, p.size, p.size);
        }
        ctx.globalAlpha = 1;
    }

    function clear() { particles = []; }
    function count() { return particles.length; }

    return { burst: burst, update: update, draw: draw, clear: clear, count: count };
})();
