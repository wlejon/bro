// screens.js — Starfighter screen state machine, built on apps/lib/screens.js.
//
// Star-tunnel background runs on title and howtoplay (lib runs the
// per-screen wiring via backgroundScreens). HUD show/hide is auto-
// toggled by lib (hudFor: ['playing']).
var N = N || {};

N.Screens = (function () {
    'use strict';

    // ----- Star tunnel background -----
    var TITLE_STAR_COUNT = 200;
    var titleStars = [];

    function bgInit(W, H) {
        titleStars.length = 0;
        for (var i = 0; i < TITLE_STAR_COUNT; i++) {
            titleStars.push({
                x: Math.random() * 2 - 1,
                y: Math.random() * 2 - 1,
                z: 0.2 + Math.random() * 0.8,
                s: 0.3 + Math.random() * 0.9,
            });
        }
    }

    function bgUpdate(dt, W, H) {
        var adv = 0.00015 * dt;
        for (var i = 0; i < titleStars.length; i++) {
            var s = titleStars[i];
            s.z -= adv;
            if (s.z <= 0.05) {
                s.x = Math.random() * 2 - 1;
                s.y = Math.random() * 2 - 1;
                s.z = 1.0;
                s.s = 0.3 + Math.random() * 0.9;
            }
        }
    }

    function bgDraw(ctx, W, H) {
        ctx.fillStyle = "#000";
        ctx.fillRect(0, 0, W, H);
        ctx.fillStyle = "#fff";
        for (var i = 0; i < titleStars.length; i++) {
            var s = titleStars[i];
            var scale = 1 / s.z;
            var px = W * 0.5 + s.x * W * 0.5 * scale;
            var py = H * 0.5 + s.y * H * 0.5 * scale;
            if (px < 0 || px >= W || py < 0 || py >= H) continue;
            ctx.globalAlpha = Math.min(1, (1 - s.z) * 1.4);
            var sz = s.s * (2 - s.z);
            ctx.fillRect(px | 0, py | 0, Math.max(1, sz | 0), Math.max(1, sz | 0));
        }
        ctx.globalAlpha = 1;
    }

    // ----- Pointer-lock callback -----
    var onPlayingChange = null;
    function setPlaying(isPlaying) {
        if (isPlaying) document.body.classList.add('playing');
        else           document.body.classList.remove('playing');
        if (onPlayingChange) onPlayingChange(isPlaying);
    }

    // ----- HUD update -----
    function updateHud() {
        var s = N.Game.getState();
        if (!s) return;
        document.getElementById('hud-score').textContent   = String(s.score);
        document.getElementById('hud-hi').textContent      = String(N.Storage.highScore);
        document.getElementById('hud-wave').textContent    = s.waveLabel;
        document.getElementById('hud-shields').textContent = s.shieldBar;
        var lock = document.getElementById('hud-lock');
        if (s.lockActive) { lock.textContent = '— LOCK —'; lock.classList.add('active'); }
        else lock.classList.remove('active');
        var radio = document.getElementById('hud-radio');
        if (s.radio) { radio.textContent = s.radio; radio.classList.add('active'); }
        else radio.classList.remove('active');
    }

    // ----- Lib screen manager -----
    var S = Screens.create({
        overlay:           '#overlay',
        onMenuMove:        function () { N.Audio.sfxMenuMove(); },
        onMenuSelect:      function () { N.Audio.sfxMenuSelect(); },
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
            if (!payload || !payload.resume) N.Game.start(W(), H());
            N.Game.setPaused(false);
            updateHud();
            setPlaying(true);
        },
        exit: function () { setPlaying(false); },
        keydown: function (key) {
            if (key === 'Escape' || key === 'p' || key === 'P') S.switchTo('pause');
            else if (key === 't' || key === 'T') N.Game.toggleTargetingComputer();
        },
        update: function (dt, w, h) {
            N.Game.update(dt, w, h);
            if (N.Game.isGameOver()) { S.switchTo('gameover'); return; }
            var s = N.Game.getState();
            if (s && s.victoryPending) { S.switchTo('victory'); return; }
            updateHud();
        },
        draw: function (ctx, w, h) { N.Game.draw(ctx, w, h); },
    });

    S.define('pause', {
        enter: function () { S.showOverlay('pause'); S.updateSelection('pause'); N.Game.setPaused(true); },
        keydown: function (key) {
            if (key === 'Escape') { S.switchTo('playing', { resume: true }); return; }
            S.menuNav('pause', key, function (idx, item) {
                var act = item.getAttribute('data-action');
                if (act === 'resume')       S.switchTo('playing', { resume: true });
                else if (act === 'restart') S.switchTo('playing');
                else if (act === 'quit')    S.switchTo('title');
            });
        },
        // Keep the last frame visible behind the pause overlay.
        draw: function (ctx, w, h) { N.Game.draw(ctx, w, h); },
    });

    S.define('gameover', {
        enter: function () {
            var s = N.Game.getState();
            var isHi = N.Storage.maybeUpdate(s ? s.score : 0);
            var lines = [
                'SCORE    ' + (s ? s.score : 0),
                'SECTOR   ' + (s ? s.waveLabel : '1-1'),
                'HI       ' + N.Storage.highScore,
            ];
            if (isHi) { lines.push(''); lines.push('NEW HIGH SCORE!'); }
            document.getElementById('gameover-stats').textContent = lines.join('\n');
            S.showOverlay('gameover'); S.updateSelection('gameover');
        },
        keydown: function (key) {
            S.menuNav('gameover', key, function (idx, item) {
                var act = item.getAttribute('data-action');
                if (act === 'restart')   S.switchTo('playing');
                else if (act === 'quit') S.switchTo('title');
            });
        },
        draw: function (ctx, w, h) { N.Game.draw(ctx, w, h); },
    });

    S.define('victory', {
        enter: function () {
            var s = N.Game.getState();
            var stats = 'CITADEL CAMPAIGN ' + (s ? s.loop : 1) + ' COMPLETE\n\n' +
                        'SCORE   ' + (s ? s.score : 0);
            document.getElementById('victory-stats').textContent = stats;
            S.showOverlay('victory'); S.updateSelection('victory');
        },
        keydown: function (key) {
            S.menuNav('victory', key, function (idx, item) {
                var act = item.getAttribute('data-action');
                if (act === 'continue') {
                    N.Game.advanceLoop();
                    S.switchTo('playing', { resume: true });
                }
            });
        },
        draw: function (ctx, w, h) { N.Game.draw(ctx, w, h); },
    });

    // ----- Public shim — preserves N.Screens.* API -----
    var _wh = { w: 1024, h: 768 };
    function W() { return _wh.w; }
    function H() { return _wh.h; }

    return {
        init: function (w, h) { _wh.w = w; _wh.h = h; },
        switchTo: function (name, w, h) { if (w) _wh.w = w; if (h) _wh.h = h; S.switchTo(name); },
        keydown:  function (key, w, h)  { if (w) _wh.w = w; if (h) _wh.h = h; S.keydown(key); },
        update:   function (dt, w, h)   { _wh.w = w; _wh.h = h; S.update(dt, w, h); },
        draw:     function (ctx, w, h)  { _wh.w = w; _wh.h = h; S.draw(ctx, w, h); },
        getName:  function () { return S.name(); },
        setOnPlayingChange: function (fn) { onPlayingChange = fn; },
    };
})();
