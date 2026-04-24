// particles.js — pop bursts, mouth puffs, shockwaves.
var SC = SC || {};

SC.FX = (function () {
    "use strict";

    var particles = [];
    var shockwaves = [];
    var floaters = []; // floating score text

    function clear() {
        particles.length = 0;
        shockwaves.length = 0;
        floaters.length = 0;
    }

    function burst(x, y, color, count, opts) {
        opts = opts || {};
        count = count || 14;
        var hex = (SC.Chain.COLORS[color] && SC.Chain.COLORS[color].hex) || "#fff";
        for (var i = 0; i < count; i++) {
            var a = Math.random() * Math.PI * 2;
            var sp = (opts.speed || 0.28) * (0.4 + Math.random() * 0.9);
            var life = (opts.life || 700) + Math.random() * 400;
            particles.push({
                x: x, y: y,
                vx: Math.cos(a) * sp,
                vy: Math.sin(a) * sp,
                life: life, maxLife: life,
                color: hex,
                size: 2 + Math.random() * 2
            });
        }
    }

    function puff(x, y) {
        for (var i = 0; i < 6; i++) {
            var a = Math.random() * Math.PI * 2;
            var sp = 0.04 + Math.random() * 0.06;
            particles.push({
                x: x, y: y,
                vx: Math.cos(a) * sp,
                vy: Math.sin(a) * sp,
                life: 500, maxLife: 500,
                color: "#9a56ff",
                size: 3
            });
        }
    }

    function shockwave(x, y, opts) {
        opts = opts || {};
        shockwaves.push({
            x: x, y: y,
            r: opts.r0 || 8,
            maxR: opts.maxR || 90,
            life: opts.life || 500,
            maxLife: opts.life || 500,
            color: opts.color || "#b56dff"
        });
    }

    function floatText(x, y, text, color) {
        floaters.push({
            x: x, y: y,
            vy: -0.05,
            life: 900, maxLife: 900,
            text: text,
            color: color || "#ffd86b"
        });
    }

    function update(dt) {
        for (var i = particles.length - 1; i >= 0; i--) {
            var p = particles[i];
            p.life -= dt;
            if (p.life <= 0) { particles.splice(i, 1); continue; }
            p.x += p.vx * dt;
            p.y += p.vy * dt;
            p.vx *= 0.98;
            p.vy *= 0.98;
        }
        for (var j = shockwaves.length - 1; j >= 0; j--) {
            var s = shockwaves[j];
            s.life -= dt;
            if (s.life <= 0) { shockwaves.splice(j, 1); continue; }
            var t = 1 - (s.life / s.maxLife);
            s.r = s.maxR * t;
        }
        for (var k = floaters.length - 1; k >= 0; k--) {
            var f = floaters[k];
            f.life -= dt;
            if (f.life <= 0) { floaters.splice(k, 1); continue; }
            f.y += f.vy * dt;
        }
    }

    function draw(ctx) {
        // Shockwaves first (under particles)
        for (var i = 0; i < shockwaves.length; i++) {
            var s = shockwaves[i];
            ctx.globalAlpha = Math.max(0, s.life / s.maxLife) * 0.7;
            ctx.strokeStyle = s.color;
            ctx.lineWidth = 3;
            ctx.beginPath();
            ctx.arc(s.x, s.y, s.r, 0, Math.PI * 2);
            ctx.stroke();
        }
        ctx.globalAlpha = 1.0;

        for (var j = 0; j < particles.length; j++) {
            var p = particles[j];
            ctx.globalAlpha = Math.max(0, p.life / p.maxLife);
            ctx.fillStyle = p.color;
            ctx.fillRect(p.x - p.size/2, p.y - p.size/2, p.size, p.size);
        }
        ctx.globalAlpha = 1.0;

        for (var k = 0; k < floaters.length; k++) {
            var f = floaters[k];
            ctx.globalAlpha = Math.max(0, f.life / f.maxLife);
            ctx.fillStyle = f.color;
            ctx.font = "bold 18px Consolas, monospace";
            ctx.textAlign = "center";
            ctx.fillText(f.text, f.x, f.y);
        }
        ctx.globalAlpha = 1.0;
    }

    function count() {
        return particles.length + shockwaves.length + floaters.length;
    }

    return {
        clear: clear,
        burst: burst,
        puff: puff,
        shockwave: shockwave,
        floatText: floatText,
        update: update,
        draw: draw,
        count: count
    };
})();
