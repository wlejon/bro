// particles.js — bubbles, splash drops, hit sparks, floating text.
'use strict';
var F = window.F = window.F || {};

F.Particles = (function () {
    var items = [];
    var bubbles = [];

    function reset() { items.length = 0; bubbles.length = 0; FX.reset(); }

    function add(p) { items.push(p); }

    function spark(x, y, color, n) {
        n = n || 8;
        for (var i = 0; i < n; i++) {
            var a = Math.random() * Math.PI * 2;
            var s = 60 + Math.random() * 120;
            items.push({
                kind: 'spark',
                x: x, y: y,
                vx: Math.cos(a) * s,
                vy: Math.sin(a) * s,
                life: 0.4 + Math.random() * 0.3,
                age: 0,
                color: color || '#ffffff'
            });
        }
    }

    function splash(x, y) {
        for (var i = 0; i < 6; i++) {
            var a = -Math.PI/2 + (Math.random()-0.5) * 1.2;
            var s = 100 + Math.random() * 80;
            items.push({
                kind: 'drop',
                x: x, y: y,
                vx: Math.cos(a) * s,
                vy: Math.sin(a) * s - 60,
                life: 0.4,
                age: 0,
                color: '#8fd6f0'
            });
        }
    }

    function floatText(x, y, text, color) {
        items.push({
            kind: 'text',
            x: x, y: y,
            vy: -30,
            life: 1.1,
            age: 0,
            text: String(text),
            color: color || '#fff4d8'
        });
    }

    function addBubble(Wd, Hd) {
        bubbles.push({
            x: 20 + Math.random() * (Wd - 40),
            y: Hd - 20,
            r: 2 + Math.random() * 4,
            vy: -(12 + Math.random() * 30),
            wobble: Math.random() * Math.PI * 2
        });
    }

    function update(dt, Wd, Hd) {
        var s = dt / 1000;
        for (var i = items.length - 1; i >= 0; i--) {
            var p = items[i];
            p.age += s;
            if (p.age >= p.life) { items.splice(i, 1); continue; }
            if (p.kind === 'spark' || p.kind === 'drop') {
                p.x += p.vx * s;
                p.y += p.vy * s;
                if (p.kind === 'drop') p.vy += 400 * s; // gravity
            } else if (p.kind === 'text') {
                p.y += p.vy * s;
            }
        }
        // bubble spawn
        if (Math.random() < dt / 300 && bubbles.length < 40) addBubble(Wd, Hd);
        for (var j = bubbles.length - 1; j >= 0; j--) {
            var b = bubbles[j];
            b.wobble += s * 3;
            b.x += Math.sin(b.wobble) * 6 * s;
            b.y += b.vy * s;
            if (b.y < 20) bubbles.splice(j, 1);
        }
        FX.tick(dt);
    }

    function draw(ctx) {
        // bubbles
        ctx.save();
        for (var i = 0; i < bubbles.length; i++) {
            var b = bubbles[i];
            ctx.globalAlpha = 0.4;
            ctx.strokeStyle = '#9fe8ff';
            ctx.lineWidth = 1;
            ctx.beginPath();
            ctx.arc(b.x, b.y, b.r, 0, Math.PI * 2);
            ctx.stroke();
        }
        ctx.globalAlpha = 1;
        // items
        for (var j = 0; j < items.length; j++) {
            var p = items[j];
            var t = 1 - (p.age / p.life);
            ctx.globalAlpha = Math.max(0, t);
            if (p.kind === 'spark') {
                ctx.fillStyle = p.color;
                ctx.fillRect(p.x - 2, p.y - 2, 4, 4);
            } else if (p.kind === 'drop') {
                ctx.fillStyle = p.color;
                ctx.beginPath();
                ctx.arc(p.x, p.y, 2.5, 0, Math.PI * 2);
                ctx.fill();
            } else if (p.kind === 'text') {
                W.Text.drawCentered(ctx, p.text, p.x, p.y, 2, p.color);
            }
        }
        ctx.globalAlpha = 1;
        ctx.restore();
    }

    function shake(mag) { FX.shake(180, mag || 6); }
    function shakeOffset() { return FX.shakeOffset(); }

    return {
        reset: reset, add: add, update: update, draw: draw,
        spark: spark, splash: splash, floatText: floatText,
        shake: shake, shakeOffset: shakeOffset,
        count: function () { return items.length; }
    };
})();

window.W = window.W || {};
