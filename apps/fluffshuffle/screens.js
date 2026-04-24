// screens.js — screen flow for fluffshuffle.
'use strict';
var G = G || {};

G.Screens = (function () {
    var Sc = null;
    var backTarget = 'title';
    var hsMode = 'classic';
    var storage = null;
    var hs = null;
    var settingsState = {
        sfxVol: 80, musicVol: 60,
        dragDead: 6, showCursor: true, eyeTrack: true,
    };

    var MODES = ['classic', 'timed', 'puzzle'];

    function init() {
        Sc = Screens.create({
            overlay: '#overlay',
            prefix: 'screen-',
            onMenuMove: function () { if (G.AppAudio) G.AppAudio.menuMove(); },
            onMenuSelect: function () {},
        });

        storage = Storage.create('fluffshuffle');
        var loaded = storage.load(settingsState);
        for (var k in loaded) if (Object.prototype.hasOwnProperty.call(loaded, k)) settingsState[k] = loaded[k];

        hs = {
            classic: Storage.highscores('fluffshuffle:classic', 10),
            timed:   Storage.highscores('fluffshuffle:timed', 10),
            puzzle:  Storage.highscores('fluffshuffle:puzzle', 10),
        };

        defineScreens();
        wireMouse();
        wireActions();
        if (G.AppAudio) G.AppAudio.init({ sfxVol: settingsState.sfxVol / 100, musicVol: settingsState.musicVol / 100 });
    }

    function saveSettings() {
        for (var k in settingsState) storage.set(k, settingsState[k]);
        storage.save();
    }

    function showHUD() { Hud.show('#hud'); }
    function hideHUD() { Hud.hide('#hud'); }

    function defineScreens() {
        Sc.define('title', {
            enter: function () { Sc.showOverlay('title'); Sc.updateSelection('title'); hideHUD(); },
            draw: function (ctx, W, H) { drawBg(ctx, W, H); },
            keydown: function (key) {
                Sc.menuNav('title', key, function (idx) {
                    if (G.AppAudio) G.AppAudio.menuSelect();
                    if (idx === 0) Sc.switchTo('modeSelect');
                    else if (idx === 1) Sc.switchTo('highScores');
                    else if (idx === 2) Sc.switchTo('howToPlay');
                    else if (idx === 3) { backTarget = 'title'; Sc.switchTo('settings'); }
                    else if (idx === 4) Sc.switchTo('credits');
                    else if (idx === 5) { if (typeof bro !== 'undefined' && bro.quit) bro.quit(); else window.close(); }
                });
            },
        });

        Sc.define('modeSelect', {
            enter: function () { Sc.showOverlay('mode-select'); Sc.updateSelection('mode-select'); hideHUD(); },
            draw: function (ctx, W, H) { drawBg(ctx, W, H); },
            keydown: function (key) {
                Sc.menuNav('mode-select', key, function (idx) {
                    if (G.AppAudio) G.AppAudio.menuSelect();
                    if (idx <= 2) {
                        G.Board.startGame(MODES[idx]);
                        Sc.switchTo('playing');
                    } else Sc.switchTo('title');
                }, { onBack: function () { Sc.switchTo('title'); } });
            },
        });

        Sc.define('settings', {
            enter: function () { Sc.showOverlay('settings'); refreshSettings(); Sc.updateSelection('settings'); },
            draw: function (ctx, W, H) { drawBg(ctx, W, H); },
            keydown: function (key) {
                Sc.menuNav('settings', key, function (idx) {
                    var items = Sc.getMenuItems('settings');
                    var act = items[idx].getAttribute('data-action');
                    if (act === 'back') { Sc.switchTo(backTarget); return; }
                    adjustSetting(1);
                }, {
                    onAdjust: function (dir) { adjustSetting(dir); },
                    onBack: function () { Sc.switchTo(backTarget); },
                });
            },
        });

        Sc.define('playing', {
            enter: function () { Sc.hideOverlay(); showHUD(); G.Board.updateHUD(); },
            exit: function () { hideHUD(); },
            update: function (dt) {
                G.Board.update(dt);
                if (G.Particles) G.Particles.update(dt);
                G.Board.updateHUD();
                var done = G.Board.isGameOver() || (G.Board.getMode() === 'puzzle' && G.Board.isFinished());
                if (done) Sc.switchTo('gameOver');
            },
            draw: function (ctx, W, H) {
                G.Board.calcLayout(W, H);
                G.Board.drawBackground(ctx, W, H);
                G.Board.drawBoard(ctx);
                if (G.Particles) G.Particles.draw(ctx);
            },
            keydown: function (key) {
                if (key === 'Escape' || key === 'p') { Sc.switchTo('paused'); return; }
                if (key === 'ArrowUp' || key === 'w') G.Board.cursorSlide(-1, 0);
                else if (key === 'ArrowDown' || key === 's') G.Board.cursorSlide(1, 0);
                else if (key === 'ArrowLeft' || key === 'a') G.Board.cursorSlide(0, -1);
                else if (key === 'ArrowRight' || key === 'd') G.Board.cursorSlide(0, 1);
                else if (key === ' ' || key === 'Enter') G.Board.cursorAction();
            },
            onMouseDown: function (px, py) { G.Board.handleMouseDown(px, py); },
            onMouseMove: function (px, py) { G.Board.handleMouseMove(px, py); },
            onMouseUp:   function (px, py) { G.Board.handleMouseUp(px, py); },
        });

        Sc.define('paused', {
            enter: function () { Sc.showOverlay('pause'); Sc.updateSelection('pause'); },
            draw: function (ctx, W, H) {
                G.Board.calcLayout(W, H);
                G.Board.drawBackground(ctx, W, H);
                G.Board.drawBoard(ctx);
            },
            keydown: function (key) {
                Sc.menuNav('pause', key, function (idx) {
                    if (G.AppAudio) G.AppAudio.menuSelect();
                    if (idx === 0) Sc.switchTo('playing');
                    else if (idx === 1) { backTarget = 'paused'; Sc.switchTo('settings'); }
                    else if (idx === 2) { G.Board.startGame(G.Board.getMode()); Sc.switchTo('playing'); }
                    else if (idx === 3) Sc.switchTo('title');
                }, { onBack: function () { Sc.switchTo('playing'); } });
            },
        });

        Sc.define('gameOver', {
            enter: function () {
                if (G.AppAudio) G.AppAudio.gameOver();
                var m = G.Board.getMode();
                var score = G.Board.getScore();
                var stats = G.Board.getStats() || {};
                var isHS = hs[m].qualifies(score);
                if (isHS && score > 0) {
                    hs[m].add({ score: score, level: G.Board.getLevel(),
                                chain: G.Board.getMaxChain(),
                                date: new Date().toISOString().slice(0, 10) });
                }
                var el = document.getElementById('gameover-stats');
                if (el) {
                    var lines = [];
                    var modeLabel = m.charAt(0).toUpperCase() + m.slice(1);
                    var title = document.querySelector('#screen-gameover .overlay-title');
                    var finished = G.Board.isFinished();
                    if (title) title.textContent = finished ? (modeLabel + ' Complete!') : 'Game Over';
                    lines.push('Score: ' + score);
                    lines.push('Level: ' + G.Board.getLevel() + '    Moves: ' + G.Board.getMoves());
                    lines.push('Popped: ' + (stats.popped || 0));
                    lines.push('Max Chain: x' + G.Board.getMaxChain());
                    lines.push('Specials: J' + (stats.jumboMade || 0) +
                               ' A' + (stats.arrowMade || 0) +
                               ' P' + (stats.prismMade || 0));
                    lines.push('Unlocks: ' + (stats.unlocks || 0));
                    if (isHS && score > 0) lines.push('\n  NEW HIGH SCORE!');
                    el.textContent = lines.join('\n');
                }
                Sc.showOverlay('gameover');
                Sc.updateSelection('gameover');
            },
            draw: function (ctx, W, H) { drawBg(ctx, W, H); },
            keydown: function (key) {
                Sc.menuNav('gameover', key, function (idx) {
                    if (G.AppAudio) G.AppAudio.menuSelect();
                    if (idx === 0) { G.Board.startGame(G.Board.getMode()); Sc.switchTo('playing'); }
                    else if (idx === 1) Sc.switchTo('highScores');
                    else if (idx === 2) Sc.switchTo('title');
                });
            },
        });

        Sc.define('highScores', {
            enter: function () { hsMode = 'classic'; Sc.showOverlay('highscores'); refreshHS(); Sc.updateSelection('highscores'); },
            draw: function (ctx, W, H) { drawBg(ctx, W, H); },
            keydown: function (key) {
                if (key === 'ArrowLeft' || key === 'ArrowRight') {
                    var idx = MODES.indexOf(hsMode);
                    idx = key === 'ArrowLeft' ? (idx - 1 + 3) % 3 : (idx + 1) % 3;
                    hsMode = MODES[idx];
                    refreshHS();
                    if (G.AppAudio) G.AppAudio.menuMove();
                    return;
                }
                Sc.menuNav('highscores', key, function () { Sc.switchTo('title'); },
                    { onBack: function () { Sc.switchTo('title'); } });
            },
        });

        Sc.define('howToPlay', {
            enter: function () { Sc.showOverlay('howtoplay'); Sc.updateSelection('howtoplay'); },
            draw: function (ctx, W, H) { drawBg(ctx, W, H); },
            keydown: function (key) {
                Sc.menuNav('howtoplay', key, function () { Sc.switchTo('title'); },
                    { onBack: function () { Sc.switchTo('title'); } });
            },
        });

        Sc.define('credits', {
            enter: function () { Sc.showOverlay('credits'); Sc.updateSelection('credits'); },
            draw: function (ctx, W, H) { drawBg(ctx, W, H); },
            keydown: function (key) {
                Sc.menuNav('credits', key, function () { Sc.switchTo('title'); },
                    { onBack: function () { Sc.switchTo('title'); } });
            },
        });
    }

    function refreshSettings() {
        var el;
        el = document.getElementById('opt-sfxVol');    if (el) el.textContent = String(settingsState.sfxVol);
        el = document.getElementById('opt-musicVol');  if (el) el.textContent = String(settingsState.musicVol);
        el = document.getElementById('opt-dragDead');  if (el) el.textContent = String(settingsState.dragDead);
        el = document.getElementById('opt-showCursor'); if (el) el.textContent = settingsState.showCursor ? 'ON' : 'OFF';
        el = document.getElementById('opt-eyeTrack');  if (el) el.textContent = settingsState.eyeTrack ? 'ON' : 'OFF';
    }

    function adjustSetting(dir) {
        var items = Sc.getMenuItems('settings');
        var sel = items[0];
        for (var i = 0; i < items.length; i++) if (items[i].classList.contains('selected')) { sel = items[i]; break; }
        var setting = sel && sel.getAttribute('data-setting');
        if (!setting) return;
        if (setting === 'sfxVol')   settingsState.sfxVol   = clamp(settingsState.sfxVol + dir * 10, 0, 100);
        else if (setting === 'musicVol') settingsState.musicVol = clamp(settingsState.musicVol + dir * 10, 0, 100);
        else if (setting === 'dragDead') settingsState.dragDead = clamp(settingsState.dragDead + dir, 2, 20);
        else if (setting === 'showCursor') settingsState.showCursor = !settingsState.showCursor;
        else if (setting === 'eyeTrack')   settingsState.eyeTrack = !settingsState.eyeTrack;
        if (G.AppAudio) G.AppAudio.setSettings({ sfxVol: settingsState.sfxVol / 100, musicVol: settingsState.musicVol / 100 });
        saveSettings();
        refreshSettings();
        if (G.AppAudio) G.AppAudio.menuMove();
    }

    function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }

    function refreshHS() {
        var tabs = { classic: 'hs-tab-classic', timed: 'hs-tab-timed', puzzle: 'hs-tab-puzzle' };
        for (var k in tabs) {
            var el = document.getElementById(tabs[k]);
            if (el) el.className = (k === hsMode) ? 'hs-tab active' : 'hs-tab';
        }
        var list = hs[hsMode].list();
        var elList = document.getElementById('hs-list');
        if (!elList) return;
        if (list.length === 0) { elList.textContent = 'No scores yet'; return; }
        var lines = [];
        for (var i = 0; i < list.length; i++) {
            var s = list[i];
            var rank = (i + 1 < 10 ? ' ' : '') + (i + 1) + '.';
            lines.push(rank + ' ' + s.score + '  Lv' + (s.level || 1) + '  x' + (s.chain || 1));
        }
        elList.textContent = lines.join('\n');
    }

    function drawBg(ctx, W, H) {
        var g = ctx.createLinearGradient(0, 0, 0, H);
        g.addColorStop(0, '#0d1326');
        g.addColorStop(1, '#231a38');
        ctx.fillStyle = g;
        ctx.fillRect(0, 0, W, H);
    }

    function wireMouse() {
        var canvas = document.getElementById('game');
        if (!canvas) return;
        canvas.addEventListener('mousedown', function (e) {
            var cur = Sc.current();
            if (cur && cur.onMouseDown) cur.onMouseDown(e.offsetX, e.offsetY);
        });
        canvas.addEventListener('mousemove', function (e) {
            var cur = Sc.current();
            if (cur && cur.onMouseMove) cur.onMouseMove(e.offsetX, e.offsetY);
        });
        canvas.addEventListener('mouseup', function (e) {
            var cur = Sc.current();
            if (cur && cur.onMouseUp) cur.onMouseUp(e.offsetX, e.offsetY);
        });
    }

    function wireActions() {
        window.addEventListener('keydown', function (e) {
            if (e.repeat && Sc.name() === 'playing') return;
            Sc.keydown(e.key);
        });
        window.addEventListener('keyup', function (e) { Sc.keyup(e.key); });
    }

    return {
        init: init,
        switchTo: function (name) { Sc.switchTo(name); },
        manager: function () { return Sc; },
        settings: function () { return settingsState; },
    };
})();
