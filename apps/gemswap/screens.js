// screens.js — screen flow for gemswap.
'use strict';
var G = G || {};

G.Screens = (function () {
    var Sc = null;
    var backTarget = 'title';
    var hsMode = 'classic';
    var storage = null;
    var hs = null;
    var settingsState = { sfxVol: 80, musicVol: 60, hintDelay: 5, dragSwap: true, showCursor: true };
    var dragFrom = null;       // when click-drag swap in progress

    // Storage keys
    var MODES = ['classic', 'timed', 'puzzle'];

    function init() {
        Sc = Screens.create({
            overlay: '#overlay',
            prefix: 'screen-',
            onMenuMove: function () { if (G.AppAudio) G.AppAudio.menuMove(); },
            onMenuSelect: function () { /* select sfx played inside callbacks */ },
        });

        storage = Storage.create('gemswap');
        var loaded = storage.load(settingsState);
        for (var k in loaded) if (Object.prototype.hasOwnProperty.call(loaded, k)) settingsState[k] = loaded[k];

        hs = {
            classic: Storage.highscores('gemswap:classic', 10),
            timed:   Storage.highscores('gemswap:timed', 10),
            puzzle:  Storage.highscores('gemswap:puzzle', 10),
        };

        defineScreens();
        wireMouse();
        wireActions();
        applySettingsToBoard();
        if (G.AppAudio) G.AppAudio.init({ sfxVol: settingsState.sfxVol / 100, musicVol: settingsState.musicVol / 100 });
    }

    function applySettingsToBoard() {
        if (G.Board && G.Board.setHintDelay) G.Board.setHintDelay(settingsState.hintDelay * 1000);
    }

    function saveSettings() { storage.load(settingsState); for (var k in settingsState) storage.set(k, settingsState[k]); storage.save(); }

    // ------------------------------------------------------------------
    function showHUD() { Hud.show('#hud'); }
    function hideHUD() { Hud.hide('#hud'); }

    function showOverlayScreen(name) { Sc.showOverlay(name); }

    function defineScreens() {
        Sc.define('title', {
            enter: function () { Sc.showOverlay('title'); Sc.updateSelection('title'); hideHUD(); },
            update: function () {},
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
                if (key === 'ArrowUp' || key === 'w') G.Board.cursorMove(-1, 0);
                else if (key === 'ArrowDown' || key === 's') G.Board.cursorMove(1, 0);
                else if (key === 'ArrowLeft' || key === 'a') G.Board.cursorMove(0, -1);
                else if (key === 'ArrowRight' || key === 'd') G.Board.cursorMove(0, 1);
                else if (key === ' ' || key === 'Enter') G.Board.cursorConfirm();
                else if (key === 'h') G.Board.clearHint();
            },
            onMouseDown: function (px, py) {
                if (!settingsState.dragSwap) { G.Board.handleClick(px, py); return; }
                var cell = pointToCell(px, py);
                dragFrom = cell;
                if (cell) G.Board.handleClick(px, py);
            },
            onMouseUp: function (px, py) {
                if (!settingsState.dragSwap || !dragFrom) return;
                var cell = pointToCell(px, py);
                if (!cell) { dragFrom = null; return; }
                var dr = cell.r - dragFrom.r, dc = cell.c - dragFrom.c;
                if (Math.abs(dr) + Math.abs(dc) === 1) {
                    // simulate two clicks: dragFrom already selected via down; now click target
                    G.Board.handleClick(px, py);
                }
                dragFrom = null;
            },
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
                    hs[m].add({ score: score, level: G.Board.getLevel(), chain: G.Board.getMaxChain(),
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
                    lines.push('Max Chain: x' + G.Board.getMaxChain());
                    lines.push('Matches: ' + (stats.matches || 0));
                    lines.push('Specials: F' + (stats.flameMade || 0) + ' S' + (stats.starMade || 0) + ' H' + (stats.hyperMade || 0));
                    if (isHS && score > 0) lines.push('\n★ NEW HIGH SCORE! ★');
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
        el = document.getElementById('opt-sfxVol'); if (el) el.textContent = String(settingsState.sfxVol);
        el = document.getElementById('opt-musicVol'); if (el) el.textContent = String(settingsState.musicVol);
        el = document.getElementById('opt-hintDelay'); if (el) el.textContent = String(settingsState.hintDelay);
        el = document.getElementById('opt-dragSwap'); if (el) el.textContent = settingsState.dragSwap ? 'ON' : 'OFF';
        el = document.getElementById('opt-showCursor'); if (el) el.textContent = settingsState.showCursor ? 'ON' : 'OFF';
    }

    function adjustSetting(dir) {
        var items = Sc.getMenuItems('settings');
        var sel = items[0]; // fallback
        for (var i = 0; i < items.length; i++) if (items[i].classList.contains('selected')) { sel = items[i]; break; }
        var setting = sel && sel.getAttribute('data-setting');
        if (!setting) return;
        if (setting === 'sfxVol') settingsState.sfxVol = clamp(settingsState.sfxVol + dir * 10, 0, 100);
        else if (setting === 'musicVol') settingsState.musicVol = clamp(settingsState.musicVol + dir * 10, 0, 100);
        else if (setting === 'hintDelay') settingsState.hintDelay = clamp(settingsState.hintDelay + dir, 1, 30);
        else if (setting === 'dragSwap') settingsState.dragSwap = !settingsState.dragSwap;
        else if (setting === 'showCursor') settingsState.showCursor = !settingsState.showCursor;
        if (G.AppAudio) G.AppAudio.setSettings({ sfxVol: settingsState.sfxVol / 100, musicVol: settingsState.musicVol / 100 });
        applySettingsToBoard();
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

    // ------------------------------------------------------------------
    function drawBg(ctx, W, H) {
        ctx.fillStyle = '#0a0612';
        ctx.fillRect(0, 0, W, H);
    }

    function pointToCell(px, py) {
        var layout = G.Board.getLayout();
        var dx = px - layout.ox, dy = py - layout.oy;
        if (dx < 0 || dy < 0) return null;
        var c = Math.floor(dx / layout.cell);
        var r = Math.floor(dy / layout.cell);
        if (r < 0 || r >= G.Board.ROWS || c < 0 || c >= G.Board.COLS) return null;
        return { r: r, c: c };
    }

    function wireMouse() {
        var canvas = document.getElementById('game');
        if (!canvas) return;
        canvas.addEventListener('mousedown', function (e) {
            var cur = Sc.current();
            if (cur && cur.onMouseDown) cur.onMouseDown(e.offsetX, e.offsetY);
        });
        canvas.addEventListener('mouseup', function (e) {
            var cur = Sc.current();
            if (cur && cur.onMouseUp) cur.onMouseUp(e.offsetX, e.offsetY);
        });
    }

    function wireActions() {
        window.addEventListener('keydown', function (e) {
            if (e.repeat) {
                if (Sc.name() === 'playing') return;
            }
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
