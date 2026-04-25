// screens.js — Touchdown screen state machine, built on apps/lib/screens.js.
//
// Title backdrop has drifting stars + ghost landers. Lib runs the
// per-screen wiring via backgroundScreens; HUD show/hide is auto-toggled
// (#hud); the secondary #telemetry panel follows playing manually since
// lib only takes one HUD selector.
var T = T || {};

T.Screens = (function () {
    'use strict';

    // ----- Background -----
    var bgStars = [];
    var bgLanders = [];

    function bgInit(W, H) {
        bgStars = [];
        for (var i = 0; i < 120; i++) {
            bgStars.push({
                x: Math.random() * W,
                y: Math.random() * H,
                b: 0.15 + Math.random() * 0.7,
                drift: 0.005 + Math.random() * 0.02,
            });
        }
        bgLanders = [];
        for (var j = 0; j < 3; j++) {
            bgLanders.push({
                x: Math.random() * W,
                y: 80 + Math.random() * (H * 0.5),
                vx: (Math.random() - 0.5) * 0.05,
                vy: 0.02 + Math.random() * 0.03,
                ang: (Math.random() - 0.5) * 0.4,
                alpha: 0.12 + Math.random() * 0.15,
            });
        }
    }

    function bgUpdate(dt, W, H) {
        for (var i = 0; i < bgStars.length; i++) {
            var s = bgStars[i];
            s.y += s.drift * dt;
            if (s.y > H) { s.y = 0; s.x = Math.random() * W; }
        }
        for (var j = 0; j < bgLanders.length; j++) {
            var L = bgLanders[j];
            L.x += L.vx * dt;
            L.y += L.vy * dt;
            if (L.y > H + 20)  { L.y = -20; L.x = Math.random() * W; }
            if (L.x < -20)     L.x = W + 20;
            if (L.x > W + 20)  L.x = -20;
        }
    }

    function bgDraw(ctx, W, H) {
        ctx.fillStyle = "#000000";
        ctx.fillRect(0, 0, W, H);
        ctx.fillStyle = "#ffffff";
        for (var i = 0; i < bgStars.length; i++) {
            var s = bgStars[i];
            ctx.globalAlpha = s.b;
            ctx.fillRect(s.x, s.y, 1, 1);
        }
        ctx.globalAlpha = 1;
        ctx.strokeStyle = "#ffffff";
        ctx.lineWidth = 1;
        for (var j = 0; j < bgLanders.length; j++) {
            var L = bgLanders[j];
            ctx.globalAlpha = L.alpha;
            ctx.save();
            ctx.translate(L.x, L.y);
            ctx.rotate(L.ang);
            ctx.beginPath();
            ctx.moveTo(0, -10);
            ctx.lineTo(-7, -2); ctx.lineTo(-7, 5);
            ctx.lineTo(7, 5);   ctx.lineTo(7, -2);
            ctx.closePath();
            ctx.stroke();
            ctx.restore();
        }
        ctx.globalAlpha = 1;
    }

    // ----- HUD -----
    function setTelemetryVisible(yes) {
        var t = document.getElementById('telemetry');
        if (t) t.style.display = yes ? 'flex' : 'none';
    }

    function updateHud() {
        var s = T.Game.getState();
        if (!s) return;
        document.getElementById('hud-score').textContent  = String(s.score);
        document.getElementById('hud-hi').textContent     = String(T.Storage.highScore);
        document.getElementById('hud-level').textContent  = String(s.level);
        document.getElementById('hud-landed').textContent = String(s.landings);
        document.getElementById('hud-fuel').textContent   = String(Math.max(0, Math.round(s.lander.fuel)));

        var L = s.lander;
        var groundY = s.H * 0.78;
        var terrain = s.terrain;
        var tx = L.x;
        if (tx < terrain.points[0].x) tx = terrain.points[0].x;
        if (tx > terrain.points[terrain.points.length - 1].x) tx = terrain.points[terrain.points.length - 1].x;
        for (var i = 1; i < terrain.points.length; i++) {
            if (terrain.points[i].x >= tx) {
                var a = terrain.points[i - 1], b = terrain.points[i];
                var tt = (tx - a.x) / (b.x - a.x || 1);
                groundY = a.y + (b.y - a.y) * tt;
                break;
            }
        }
        var alt = Math.max(0, Math.round(groundY - L.y));
        document.getElementById('tel-alt').textContent  = String(alt);
        document.getElementById('tel-hvel').textContent = L.vx.toFixed(2);
        document.getElementById('tel-vvel').textContent = L.vy.toFixed(2);
    }

    // ----- Lib screen manager -----
    var S = Screens.create({
        overlay:           '#overlay',
        onMenuMove:        function () { T.Audio.sfxMenuMove(); },
        onMenuSelect:      function () { T.Audio.sfxMenuSelect(); },
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
            var skipStart = payload && (payload.resume || payload.advance);
            if (!skipStart) T.Game.start(W(), H());
            T.Game.setPaused(false);
            setTelemetryVisible(true);
            updateHud();
        },
        exit: function () { setTelemetryVisible(false); },
        keydown: function (key) { if (key === 'Escape' || key === 'p' || key === 'P') S.switchTo('pause'); },
        update: function (dt, w, h) {
            T.Game.update(dt, w, h);
            if (T.Game.isLanded())   { S.switchTo('landed');   return; }
            if (T.Game.isGameOver()) { S.switchTo('gameover'); return; }
            updateHud();
        },
        draw: function (ctx, w, h) {
            ctx.fillStyle = "#000000"; ctx.fillRect(0, 0, w, h);
            T.Game.draw(ctx, w, h);
        },
    });

    S.define('pause', {
        enter: function () { S.showOverlay('pause'); S.updateSelection('pause'); T.Game.setPaused(true); },
        keydown: function (key) {
            if (key === 'Escape') { S.switchTo('playing', { resume: true }); return; }
            S.menuNav('pause', key, function (idx, item) {
                var act = item.getAttribute('data-action');
                if (act === 'resume')       S.switchTo('playing', { resume: true });
                else if (act === 'restart') S.switchTo('playing');
                else if (act === 'quit')    S.switchTo('title');
            });
        },
        draw: function (ctx, w, h) {
            ctx.fillStyle = "#000000"; ctx.fillRect(0, 0, w, h);
            T.Game.draw(ctx, w, h);
        },
    });

    S.define('landed', {
        enter: function () {
            var s = T.Game.getState();
            var lines = [
                'LEVEL        ' + s.level,
                'PAD WIDTH    ' + s.lastLandingPadWidth,
                'BONUS        +' + s.lastLandingBonus,
                'SCORE        ' + s.score,
                'FUEL LEFT    ' + Math.round(s.lander.fuel),
            ];
            document.getElementById('landed-stats').textContent = lines.join('\n');
            S.showOverlay('landed'); S.updateSelection('landed');
        },
        keydown: function (key) {
            S.menuNav('landed', key, function (idx, item) {
                if (item.getAttribute('data-action') === 'next') {
                    T.Game.advanceLevel();
                    S.switchTo('playing', { advance: true });
                }
            });
        },
        draw: function (ctx, w, h) {
            ctx.fillStyle = "#000000"; ctx.fillRect(0, 0, w, h);
            T.Game.draw(ctx, w, h);
        },
    });

    S.define('gameover', {
        enter: function () {
            var s = T.Game.getState();
            var isHi = T.Storage.maybeUpdate(s ? s.score : 0);
            var lines = [
                'SCORE     ' + (s ? s.score : 0),
                'LEVEL     ' + (s ? s.level : 1),
                'LANDED    ' + (s ? s.landings : 0),
                'HI        ' + T.Storage.highScore,
            ];
            if (isHi) { lines.push(''); lines.push('NEW HIGH SCORE!'); }
            document.getElementById('gameover-stats').textContent = lines.join('\n');
            document.getElementById('gameover-title').textContent = 'CRASHED';
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
            T.Game.draw(ctx, w, h);
        },
    });

    // ----- Public shim — preserves T.Screens.* API -----
    var _wh = { w: 900, h: 800 };
    function W() { return _wh.w; }
    function H() { return _wh.h; }

    return {
        init:     function (w, h) { _wh.w = w; _wh.h = h; },
        switchTo: function (name, w, h) { if (w) _wh.w = w; if (h) _wh.h = h; S.switchTo(name); },
        keydown:  function (key, w, h)  { if (w) _wh.w = w; if (h) _wh.h = h; S.keydown(key); },
        keyup:    function () {},
        update:   function (dt, w, h)   { _wh.w = w; _wh.h = h; S.update(dt, w, h); },
        draw:     function (ctx, w, h)  { _wh.w = w; _wh.h = h; S.draw(ctx, w, h); },
        getName:  function () { return S.name(); },
    };
})();
