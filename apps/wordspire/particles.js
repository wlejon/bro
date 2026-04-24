// particles.js — sparkle particles, shake, action text.
'use strict';
var W = window.W = window.W || {};

W.Particles = (function () {
    var parts = [];
    var shakeTimer = 0, shakeMag = 0;
    var actionTextTimer = 0;

    function spawn(x, y, count, color, opts) {
        opts = opts || {};
        for (var i = 0; i < count; i++) {
            var life = (opts.life || 420) + Math.random() * (opts.lifeVar || 300);
            var ang  = Math.random() * Math.PI * 2;
            var spd  = (opts.speed || 2.5) + Math.random() * (opts.speedVar || 2.5);
            parts.push({
                x: x, y: y,
                vx: Math.cos(ang) * spd + (opts.vx || 0),
                vy: Math.sin(ang) * spd + (opts.vy || 0),
                life: life, maxLife: life,
                size: (opts.size || 2) + Math.random() * (opts.sizeVar || 3),
                color: color,
                gravity: opts.gravity != null ? opts.gravity : 0.14
            });
        }
    }

    function burst(x, y, color) {
        spawn(x, y, 14, color, { speed: 3, speedVar: 3, life: 450, lifeVar: 300 });
    }

    function shake(duration, mag) {
        shakeTimer = duration;
        shakeMag = mag;
    }

    function shakeOffset() {
        if (shakeTimer <= 0) return { x: 0, y: 0 };
        var m = (shakeTimer / 300) * shakeMag;
        return { x: (Math.random() - 0.5) * m, y: (Math.random() - 0.5) * m };
    }

    function showAction(text) {
        var el = document.getElementById('action-text');
        if (el) { el.textContent = text; el.style.display = 'block'; }
        actionTextTimer = 1100;
    }

    function update(dt) {
        for (var i = parts.length - 1; i >= 0; i--) {
            var p = parts[i];
            p.life -= dt;
            if (p.life <= 0) { parts.splice(i, 1); continue; }
            p.x += p.vx;
            p.y += p.vy;
            p.vy += p.gravity;
        }
        if (shakeTimer > 0) shakeTimer -= dt;
        if (actionTextTimer > 0) {
            actionTextTimer -= dt;
            if (actionTextTimer <= 0) {
                var el = document.getElementById('action-text');
                if (el) el.style.display = 'none';
            }
        }
    }

    function clear() {
        parts.length = 0;
        shakeTimer = 0;
        actionTextTimer = 0;
        var el = document.getElementById('action-text');
        if (el) el.style.display = 'none';
    }

    function drawParticles(ctx) {
        for (var i = 0; i < parts.length; i++) {
            var p = parts[i];
            ctx.globalAlpha = Math.max(0, p.life / p.maxLife);
            ctx.fillStyle = p.color;
            ctx.fillRect(p.x - p.size / 2, p.y - p.size / 2, p.size, p.size);
        }
        ctx.globalAlpha = 1.0;
    }

    return {
        spawn: spawn, burst: burst, shake: shake, shakeOffset: shakeOffset,
        showAction: showAction,
        update: update, clear: clear,
        draw: drawParticles,
        count: function () { return parts.length; }
    };
})();
