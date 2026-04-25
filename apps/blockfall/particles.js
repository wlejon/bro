// particles.js — Particle effects, flash cells, screen shake, action text
var T = T || {};

T.FX = {
    particles: [],
    flashCells: [],
    lineClearAnim: null,

    spawn: function(x, y, count, color, opts) {
        opts = opts || {};
        for (var i = 0; i < count; i++) {
            var life = (opts.life || 400) + Math.random() * (opts.lifeVar || 300);
            this.particles.push({
                x: x, y: y,
                vx: (opts.vx || 0) + (Math.random() - 0.5) * (opts.spread || 4),
                vy: (opts.vy || 0) + (Math.random() - 1) * (opts.spreadY || 3),
                life: life, maxLife: life,
                size: (opts.size || 2) + Math.random() * (opts.sizeVar || 3),
                color: color
            });
        }
    },

    flash: function(r, c, duration, color) {
        this.flashCells.push({ r: r, c: c, timer: duration, color: color });
    },

    shake: function(duration, magnitude) { FX.shake(duration, magnitude); },

    showText: function(text) { FX.toast(text); },

    startLineClear: function(rows) {
        this.lineClearAnim = { rows: rows, timer: 0, duration: 250 };
    },

    update: function(dt) {
        var p = this.particles;
        for (var i = p.length - 1; i >= 0; i--) {
            p[i].life -= dt;
            if (p[i].life <= 0) { p.splice(i, 1); continue; }
            p[i].x += p[i].vx;
            p[i].y += p[i].vy;
            p[i].vy += 0.15;
        }
        var fc = this.flashCells;
        for (var i = fc.length - 1; i >= 0; i--) {
            fc[i].timer -= dt;
            if (fc[i].timer <= 0) fc.splice(i, 1);
        }
        FX.tick(dt);
        if (this.lineClearAnim) {
            this.lineClearAnim.timer += dt;
            if (this.lineClearAnim.timer >= this.lineClearAnim.duration)
                this.lineClearAnim = null;
        }
    },

    getShakeOffset: function() { return FX.shakeOffset(); },

    clear: function() {
        this.particles.length = 0;
        this.flashCells.length = 0;
        this.lineClearAnim = null;
        FX.reset();
    },

    drawParticles: function(ctx) {
        var p = this.particles;
        for (var i = 0; i < p.length; i++) {
            ctx.globalAlpha = p[i].life / p[i].maxLife;
            ctx.fillStyle = p[i].color;
            ctx.fillRect(p[i].x - p[i].size / 2, p[i].y - p[i].size / 2,
                         p[i].size, p[i].size);
        }
        ctx.globalAlpha = 1.0;
    },

    drawFlashCells: function(ctx, BOARD_X, BOARD_Y, CELL) {
        var fc = this.flashCells;
        for (var i = 0; i < fc.length; i++) {
            ctx.globalAlpha = (fc[i].timer / 200) * 0.5;
            ctx.fillStyle = "#ffffff";
            ctx.fillRect(BOARD_X + fc[i].c * CELL, BOARD_Y + fc[i].r * CELL, CELL, CELL);
        }
        ctx.globalAlpha = 1.0;
    },

    drawLineClearFlash: function(ctx, BOARD_X, BOARD_Y, BOARD_W, CELL) {
        if (!this.lineClearAnim) return;
        var progress = this.lineClearAnim.timer / this.lineClearAnim.duration;
        var flashAlpha = Math.sin(progress * Math.PI * 3) * 0.5;
        if (flashAlpha > 0) {
            ctx.globalAlpha = flashAlpha;
            ctx.fillStyle = "#ffffff";
            for (var i = 0; i < this.lineClearAnim.rows.length; i++) {
                ctx.fillRect(BOARD_X, BOARD_Y, BOARD_W, CELL);
            }
            ctx.globalAlpha = 1.0;
        }
    },

    hideActionText: function() { FX.reset(); }
};
