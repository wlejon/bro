// particles.js — Particle effects for explosions and ship trail
var A = A || {};

A.FX = {
    particles: [],

    spawn: function(x, y, count, opts) {
        opts = opts || {};
        for (var i = 0; i < count; i++) {
            var a = Math.random() * Math.PI * 2;
            var sp = (opts.speed || 0.15) * (0.5 + Math.random());
            var life = (opts.life || 600) + Math.random() * (opts.lifeVar || 400);
            this.particles.push({
                x: x, y: y,
                vx: Math.cos(a) * sp + (opts.vx || 0),
                vy: Math.sin(a) * sp + (opts.vy || 0),
                life: life, maxLife: life,
                color: opts.color || "#ffffff"
            });
        }
    },

    update: function(dt) {
        var p = this.particles;
        for (var i = p.length - 1; i >= 0; i--) {
            p[i].life -= dt;
            if (p[i].life <= 0) { p.splice(i, 1); continue; }
            p[i].x += p[i].vx * dt;
            p[i].y += p[i].vy * dt;
        }
    },

    draw: function(ctx, W, H) {
        var p = this.particles;
        for (var i = 0; i < p.length; i++) {
            var pr = p[i];
            var x = pr.x, y = pr.y;
            // wrap
            if (x < 0) x += W; else if (x >= W) x -= W;
            if (y < 0) y += H; else if (y >= H) y -= H;
            ctx.globalAlpha = Math.max(0, pr.life / pr.maxLife);
            ctx.fillStyle = pr.color;
            ctx.fillRect(x - 1, y - 1, 2, 2);
        }
        ctx.globalAlpha = 1.0;
    },

    clear: function() {
        this.particles.length = 0;
    }
};
