// particles.js — cascade burst + tiny fur-mote effects.
'use strict';
var G = G || {};

G.Particles = (function () {
    var particles = [];
    var labels = [];   // floating score / chain popups

    function popLabel(x, y, text, color, big) {
        labels.push({
            x: x, y: y,
            vy: -38 - (big ? 22 : 0),
            life: big ? 1100 : 800,
            age: 0,
            text: String(text),
            color: color || '#ffe9b0',
            big: !!big,
        });
    }

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

        var keptL = [];
        for (var j = 0; j < labels.length; j++) {
            var L = labels[j];
            L.age += dt;
            if (L.age >= L.life) continue;
            L.y += L.vy * dt / 1000;
            L.vy *= Math.pow(0.94, dt / 16);
            keptL.push(L);
        }
        labels = keptL;
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

    function drawLabels(ctx) {
        for (var i = 0; i < labels.length; i++) {
            var L = labels[i];
            var t = L.age / L.life;
            var alpha = t < 0.7 ? 1 : (1 - (t - 0.7) / 0.3);
            var scale = L.big
                ? (t < 0.18 ? 0.5 + (t / 0.18) * 0.7 : 1.2 - Math.min(0.2, (t - 0.18) * 0.4))
                : (t < 0.15 ? 0.6 + (t / 0.15) * 0.5 : 1.1);
            ctx.save();
            ctx.translate(L.x, L.y);
            ctx.scale(scale, scale);
            ctx.globalAlpha = Math.max(0, alpha);
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.font = (L.big ? 'bold 30px ' : 'bold 20px ') + 'sans-serif';
            ctx.lineWidth = L.big ? 4 : 3;
            ctx.strokeStyle = 'rgba(0, 0, 0, 0.65)';
            ctx.strokeText(L.text, 0, 0);
            ctx.fillStyle = L.color;
            ctx.fillText(L.text, 0, 0);
            ctx.restore();
        }
        ctx.globalAlpha = 1;
    }

    function clear() { particles = []; labels = []; }
    function count() { return particles.length + labels.length; }

    return {
        burst: burst,
        popLabel: popLabel,
        update: update,
        draw: function (ctx) { draw(ctx); drawLabels(ctx); },
        clear: clear,
        count: count,
    };
})();
