// particles.js — cascade burst + tiny fur-mote effects.
'use strict';
var G = G || {};

G.Particles = (function () {
    var particles = [];

    function burst(x, y, color, count) {
        count = count || 8;
        for (var i = 0; i < count; i++) {
            var ang = Math.random() * Math.PI * 2;
            var spd = 60 + Math.random() * 180;
            particles.push({
                x: x, y: y,
                vx: Math.cos(ang) * spd,
                vy: Math.sin(ang) * spd - 40,
                life: 650 + Math.random() * 450,
                age: 0,
                color: color,
                size: 2 + Math.random() * 3,
                spin: (Math.random() - 0.5) * 6,
                rot: Math.random() * Math.PI * 2,
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
            p.vy += 260 * dt / 1000;     // gravity
            p.vx *= Math.pow(0.92, dt / 16);
            p.rot += p.spin * dt / 1000;
            kept.push(p);
        }
        particles = kept;
    }

    function draw(ctx) {
        for (var i = 0; i < particles.length; i++) {
            var p = particles[i];
            var a = 1 - (p.age / p.life);
            ctx.save();
            ctx.translate(p.x, p.y);
            ctx.rotate(p.rot);
            ctx.globalAlpha = Math.max(0, a);
            ctx.fillStyle = p.color;
            // Draw as a fuzzy tuft: diamond + inner dot.
            ctx.beginPath();
            ctx.moveTo(0, -p.size);
            ctx.lineTo(p.size, 0);
            ctx.lineTo(0, p.size);
            ctx.lineTo(-p.size, 0);
            ctx.closePath();
            ctx.fill();
            ctx.restore();
        }
        ctx.globalAlpha = 1;
    }

    function clear() { particles = []; }
    function count() { return particles.length; }

    return { burst: burst, update: update, draw: draw, clear: clear, count: count };
})();
