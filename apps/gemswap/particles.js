// particles.js — Gem shatter shards + score labels.
'use strict';
var G = G || {};

G.Particles = (function () {
    var shards = [];
    var labels = [];

    // Shatter burst — sharp glittering shards in the gem's color, with an
    // outer rim shard layer that's brighter (highlight) and a darker core.
    function burst(x, y, color, count) {
        count = count || 10;
        for (var i = 0; i < count; i++) {
            var ang = Math.random() * Math.PI * 2;
            var spd = 90 + Math.random() * 220;
            shards.push({
                x: x, y: y,
                vx: Math.cos(ang) * spd,
                vy: Math.sin(ang) * spd - 30,
                life: 480 + Math.random() * 360,
                age: 0,
                color: color,
                size: 2.5 + Math.random() * 3,
                spin: (Math.random() - 0.5) * 14,
                rot: Math.random() * Math.PI * 2,
                kind: i & 1 ? 'rim' : 'core',
            });
        }
    }

    function popLabel(x, y, text, color, big) {
        labels.push({
            x: x, y: y,
            vy: -36 - (big ? 18 : 0),
            life: big ? 1000 : 720,
            age: 0,
            text: String(text),
            color: color || '#ffe9b0',
            big: !!big,
        });
    }

    function update(dt) {
        var keptS = [];
        for (var i = 0; i < shards.length; i++) {
            var p = shards[i];
            p.age += dt;
            if (p.age >= p.life) continue;
            p.x += p.vx * dt / 1000;
            p.y += p.vy * dt / 1000;
            p.vy += 360 * dt / 1000;     // gravity
            p.vx *= Math.pow(0.93, dt / 16);
            p.rot += p.spin * dt / 1000;
            keptS.push(p);
        }
        shards = keptS;

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

    function drawShards(ctx) {
        for (var i = 0; i < shards.length; i++) {
            var p = shards[i];
            var a = 1 - (p.age / p.life);
            ctx.save();
            ctx.translate(p.x, p.y);
            ctx.rotate(p.rot);
            ctx.globalAlpha = Math.max(0, a);
            // Sharp triangular shard.
            var s = p.size;
            ctx.beginPath();
            ctx.moveTo(0, -s * 1.4);
            ctx.lineTo(s * 0.7, s * 0.6);
            ctx.lineTo(-s * 0.7, s * 0.6);
            ctx.closePath();
            ctx.fillStyle = p.color;
            ctx.fill();
            if (p.kind === 'rim') {
                ctx.strokeStyle = 'rgba(255, 255, 255, 0.9)';
                ctx.lineWidth = 0.8;
                ctx.stroke();
            }
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
            ctx.font = (L.big ? 'bold 30px ' : 'bold 18px ') + 'sans-serif';
            ctx.lineWidth = L.big ? 4 : 3;
            ctx.strokeStyle = 'rgba(0, 0, 0, 0.7)';
            ctx.strokeText(L.text, 0, 0);
            // Soft glow
            ctx.shadowColor = L.color;
            ctx.shadowBlur = L.big ? 14 : 8;
            ctx.fillStyle = '#ffffff';
            ctx.fillText(L.text, 0, 0);
            ctx.restore();
        }
        ctx.globalAlpha = 1;
    }

    function clear() { shards = []; labels = []; }
    function count() { return shards.length + labels.length; }

    return {
        burst: burst,
        popLabel: popLabel,
        update: update,
        draw: function (ctx) { drawShards(ctx); drawLabels(ctx); },
        clear: clear,
        count: count,
    };
})();
