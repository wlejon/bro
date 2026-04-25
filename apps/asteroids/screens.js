// screens.js — Asteroids screen state machine, built on apps/lib/screens.js.
//
// The animated drifting-asteroid background runs on title and howtoplay
// (lib handles the per-screen wiring via backgroundScreens). HUD visibility
// is auto-toggled by lib (hudFor: ['playing']).
var A = A || {};

A.Screens = (function () {
    'use strict';

    // ----- Background asteroids -----
    var bg = [];

    function makeShape(radius) {
        var pts = [];
        var n = 10 + Math.floor(Math.random() * 4);
        for (var i = 0; i < n; i++) {
            var a = (i / n) * Math.PI * 2;
            var r = radius * (0.75 + Math.random() * 0.45);
            pts.push({ x: Math.cos(a) * r, y: Math.sin(a) * r });
        }
        return pts;
    }

    function bgInit(W, H) {
        bg = [];
        for (var i = 0; i < 8; i++) {
            var radius = 18 + Math.random() * 30;
            var a = Math.random() * Math.PI * 2;
            var sp = 0.02 + Math.random() * 0.04;
            bg.push({
                x: Math.random() * W, y: Math.random() * H,
                vx: Math.cos(a) * sp, vy: Math.sin(a) * sp,
                rot: Math.random() * Math.PI * 2,
                rotSpeed: (Math.random() - 0.5) * 0.0008,
                radius: radius, shape: makeShape(radius),
                alpha: 0.15 + Math.random() * 0.15,
            });
        }
    }

    function bgUpdate(dt, W, H) {
        for (var i = 0; i < bg.length; i++) {
            var a = bg[i];
            a.x += a.vx * dt; a.y += a.vy * dt; a.rot += a.rotSpeed * dt;
            if (a.x < -a.radius)        a.x = W + a.radius;
            else if (a.x > W + a.radius) a.x = -a.radius;
            if (a.y < -a.radius)        a.y = H + a.radius;
            else if (a.y > H + a.radius) a.y = -a.radius;
        }
    }

    function bgDraw(ctx, W, H) {
        ctx.fillStyle = "#000000";
        ctx.fillRect(0, 0, W, H);
        ctx.strokeStyle = "#ffffff";
        ctx.lineWidth = 1;
        for (var i = 0; i < bg.length; i++) {
            var a = bg[i];
            ctx.globalAlpha = a.alpha;
            ctx.beginPath();
            var c = Math.cos(a.rot), s = Math.sin(a.rot);
            for (var j = 0; j < a.shape.length; j++) {
                var p = a.shape[j];
                var px = a.x + p.x * c - p.y * s;
                var py = a.y + p.x * s + p.y * c;
                if (j === 0) ctx.moveTo(px, py); else ctx.lineTo(px, py);
            }
            ctx.closePath();
            ctx.stroke();
        }
        ctx.globalAlpha = 1;
    }

    // ----- HUD -----
    function updateHud() {
        var s = A.Game.getState();
        if (!s) return;
        document.getElementById("hud-score").textContent = String(s.score);
        document.getElementById("hud-hi").textContent    = String(A.Storage.highScore);
        document.getElementById("hud-wave").textContent  = String(s.wave);
        document.getElementById("hud-lives").textContent = String(s.lives);
    }

    // ----- Lib screen manager -----
    var S = Screens.create({
        overlay:           '#overlay',
        onMenuMove:        function () { A.Audio.sfxMenuMove(); },
        onMenuSelect:      function () { A.Audio.sfxMenuSelect(); },
        backgroundScreens: ['title', 'howtoplay'],
        backgroundInit:    bgInit,
        backgroundUpdate:  bgUpdate,
        backgroundDraw:    bgDraw,
        hudSelector:       '#hud',
        hudFor:            ['playing'],
    });

    // ----- Screen definitions -----
    S.define('title', {
        enter: function () { S.showOverlay('title'); S.updateSelection('title'); },
        keydown: function (key) {
            S.menuNav('title', key, function (idx, item) {
                var act = item.getAttribute('data-action');
                if (act === 'play')           S.switchTo('playing');
                else if (act === 'howtoplay') S.switchTo('howtoplay');
                else if (act === 'quit')      { try { window.close && window.close(); } catch (e) {} }
            });
        },
    });

    S.define('howtoplay', {
        enter: function () { S.showOverlay('howtoplay'); S.updateSelection('howtoplay'); },
        keydown: function (key) {
            if (key === 'Escape') { S.switchTo('title'); return; }
            S.menuNav('howtoplay', key, function (idx, item) {
                if (item.getAttribute('data-action') === 'back') S.switchTo('title');
            });
        },
    });

    S.define('playing', {
        enter: function (payload) {
            S.hideOverlay();
            if (!payload || !payload.resume) A.Game.start(W(), H());
            A.Game.setPaused(false);
            updateHud();
        },
        keydown: function (key) { if (key === 'Escape' || key === 'p' || key === 'P') S.switchTo('pause'); },
        update: function (dt, w, h) {
            A.Game.update(dt, w, h);
            if (A.FX) A.FX.update(dt);
            if (A.Game.isGameOver()) { S.switchTo('gameover'); return; }
            updateHud();
        },
        draw: function (ctx, w, h) {
            ctx.fillStyle = "#000000"; ctx.fillRect(0, 0, w, h);
            A.Game.draw(ctx, w, h);
            if (A.FX) A.FX.draw(ctx, w, h);
        },
    });

    S.define('pause', {
        enter: function () { S.showOverlay('pause'); S.updateSelection('pause'); A.Game.setPaused(true); },
        keydown: function (key) {
            if (key === 'Escape') { S.switchTo('playing', { resume: true }); return; }
            S.menuNav('pause', key, function (idx, item) {
                var act = item.getAttribute('data-action');
                if (act === 'resume')       S.switchTo('playing', { resume: true });
                else if (act === 'restart') S.switchTo('playing');
                else if (act === 'quit')    S.switchTo('title');
            });
        },
        // Keep last frame on screen while paused.
        draw: function (ctx, w, h) {
            ctx.fillStyle = "#000000"; ctx.fillRect(0, 0, w, h);
            A.Game.draw(ctx, w, h);
            if (A.FX) A.FX.draw(ctx, w, h);
        },
    });

    S.define('gameover', {
        enter: function () {
            var s = A.Game.getState();
            var isHi = A.Storage.maybeUpdate(s ? s.score : 0);
            var lines = [
                "SCORE   " + (s ? s.score : 0),
                "WAVE    " + (s ? s.wave : 1),
                "HI      " + A.Storage.highScore,
            ];
            if (isHi) { lines.push(""); lines.push("NEW HIGH SCORE!"); }
            document.getElementById("gameover-stats").textContent = lines.join("\n");
            S.showOverlay('gameover'); S.updateSelection('gameover');
        },
        keydown: function (key) {
            S.menuNav('gameover', key, function (idx, item) {
                var act = item.getAttribute('data-action');
                if (act === 'restart')   S.switchTo('playing');
                else if (act === 'quit') S.switchTo('title');
            });
        },
        draw: function (ctx, w, h) {
            ctx.fillStyle = "#000000"; ctx.fillRect(0, 0, w, h);
            A.Game.draw(ctx, w, h);
        },
    });

    // ----- Public shim — preserves A.Screens.* API used by app.js -----
    // app.js stores W/H by querying its own getW/getH on each call. We need
    // a similar source for screens that need canvas dimensions in enter().
    var _wh = { w: 900, h: 800 };
    function W() { return _wh.w; }
    function H() { return _wh.h; }

    return {
        init: function (w, h) { _wh.w = w; _wh.h = h; },
        switchTo: function (name, w, h) { if (w) _wh.w = w; if (h) _wh.h = h; S.switchTo(name); },
        keydown: function (key, w, h) { if (w) _wh.w = w; if (h) _wh.h = h; S.keydown(key); },
        keyup:   function () {},
        update:  function (dt, w, h) { _wh.w = w; _wh.h = h; S.update(dt, w, h); },
        draw:    function (ctx, w, h) { _wh.w = w; _wh.h = h; S.draw(ctx, w, h); },
        getName: function () { return S.name(); },
    };
})();
