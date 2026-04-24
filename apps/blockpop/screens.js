// screens.js — screen state machine + per-screen input handling.
'use strict';
var G = window.G = window.G || {};

G.Screens = (function () {
    var mgr = null;
    var hsMode = 'classic';
    var backTarget = 'title';
    var bgT = 0;

    function init() {
        mgr = Screens.create({
            overlay: '#overlay',
            prefix: 'screen-',
            onMenuMove: function () { G.Audio.menuMove(); },
            onMenuSelect: function () { G.Audio.menuSelect(); }
        });

        mgr.define('title',      titleScreen);
        mgr.define('modeSelect', modeSelect);
        mgr.define('settings',   settingsScreen);
        mgr.define('playing',    playing);
        mgr.define('paused',     paused);
        mgr.define('gameover',   gameover);
        mgr.define('highscores', highscores);
        mgr.define('howtoplay',  howToPlay);
        mgr.define('credits',    credits);
    }

    function manager() { return mgr; }
    function switchTo(n, payload) { mgr.switchTo(n, payload); }

    function drawBg(ctx, W, H) {
        bgT += 16;
        ctx.fillStyle = '#050810';
        ctx.fillRect(0, 0, W, H);
        // gradient sky
        ctx.globalAlpha = 0.35;
        ctx.fillStyle = '#0a2238';
        ctx.fillRect(0, H * 0.6, W, H * 0.4);
        ctx.globalAlpha = 1.0;
        // stars
        for (var i = 0; i < 60; i++) {
            var sx = ((i * 83) + (bgT * 0.02)) % W;
            var sy = ((i * 137) + (bgT * 0.01)) % H;
            ctx.globalAlpha = 0.12 + ((i % 5) * 0.03);
            ctx.fillStyle = '#b0d8ff';
            ctx.fillRect(sx, sy, 2, 2);
        }
        // drifting blocks
        for (var j = 0; j < 18; j++) {
            var bx = ((j * 71 + bgT * 0.04) % (W + 40));
            var by = ((j * 193 + bgT * 0.025) % (H + 40));
            ctx.globalAlpha = 0.08;
            var cidx = 1 + (j % G.Board.NUM_COLORS);
            ctx.fillStyle = G.Board.COLORS[cidx];
            ctx.fillRect(bx - 16, by - 16, 32, 32);
        }
        ctx.globalAlpha = 1.0;
    }

    function showHUD() { Hud.show('#hud'); }
    function hideHUD() { Hud.hide('#hud'); }

    // ---- TITLE ---------------------------------------------------------
    var titleScreen = {
        enter: function () { mgr.showOverlay('title'); hideHUD(); },
        exit: function () {},
        update: function (dt) {},
        draw: drawBg,
        keydown: function (key) {
            mgr.menuNav('title', key, function (idx, el) {
                var action = el && el.getAttribute('data-action');
                if (action === 'play') switchTo('modeSelect');
                else if (action === 'highscores') switchTo('highscores');
                else if (action === 'howtoplay') switchTo('howtoplay');
                else if (action === 'settings') { backTarget = 'title'; switchTo('settings'); }
                else if (action === 'credits') switchTo('credits');
                else if (action === 'quit') { try { window.close(); } catch (e) {} }
            });
        }
    };

    // ---- MODE SELECT --------------------------------------------------
    var modeSelect = {
        enter: function () { mgr.showOverlay('mode-select'); hideHUD(); },
        exit: function () {},
        update: function () {},
        draw: drawBg,
        keydown: function (key) {
            mgr.menuNav('mode-select', key, function (idx, el) {
                var m = el && el.getAttribute('data-mode');
                var act = el && el.getAttribute('data-action');
                if (m) {
                    G.Board.startGame(m);
                    switchTo('playing');
                } else if (act === 'back') switchTo('title');
            }, { onBack: function () { switchTo('title'); } });
        }
    };

    // ---- SETTINGS ----------------------------------------------------
    var settingsScreen = {
        enter: function () {
            mgr.showOverlay('settings');
            this.refresh();
        },
        exit: function () {},
        update: function () {},
        draw: drawBg,
        refresh: function () {
            var s = G.Storage.settings;
            Hud.text('#opt-sfxVol', String(s.sfxVol));
            Hud.text('#opt-musicVol', String(s.musicVol));
            Hud.text('#opt-riseSpeed', (s.riseSpeed / 10).toFixed(1));
            Hud.text('#opt-colorBlind', s.colorBlind ? 'ON' : 'OFF');
        },
        adjust: function (dir) {
            var items = mgr.getMenuItems('settings');
            var idx = 0;
            for (var i = 0; i < items.length; i++) if (items[i].classList.contains('selected')) { idx = i; break; }
            var item = items[idx];
            if (!item) return;
            var key = item.getAttribute('data-setting');
            var s = G.Storage.settings;
            if (!key) return;
            if (key === 'sfxVol') {
                s.sfxVol = Math.max(0, Math.min(100, s.sfxVol + dir * 10));
                G.Audio.setSfxVol(s.sfxVol / 100);
            } else if (key === 'musicVol') {
                s.musicVol = Math.max(0, Math.min(100, s.musicVol + dir * 10));
                SFX.setMusicVol(s.musicVol / 100);
            } else if (key === 'riseSpeed') {
                s.riseSpeed = Math.max(5, Math.min(20, s.riseSpeed + dir));
            } else if (key === 'colorBlind') {
                s.colorBlind = !s.colorBlind;
            }
            G.Storage.save();
            this.refresh();
            G.Audio.menuMove();
        },
        keydown: function (key) {
            var self = this;
            mgr.menuNav('settings', key, function (idx, el) {
                if (el && el.getAttribute('data-action') === 'back') switchTo(backTarget);
                else self.adjust(1);
            }, {
                onAdjust: function (d) { self.adjust(d); },
                onBack:   function () { switchTo(backTarget); }
            });
        }
    };

    // ---- PLAYING -----------------------------------------------------
    var playing = {
        enter: function () {
            mgr.hideOverlay();
            showHUD();
            G.Board.updateHUD();
        },
        exit: function () { hideHUD(); },
        update: function (dt, W, H) {
            G.Board.tick(dt);
            G.Particles.update(dt);
            if (G.Board.isGameOver() || G.Board.isFinished()) {
                // Delay a beat so particles play.
                switchTo('gameover');
            }
        },
        draw: function (ctx, W, H) {
            var shake = G.Particles.shakeOffset();
            ctx.save();
            ctx.translate(shake.x, shake.y);
            G.Board.draw(ctx, W, H, performance.now());
            ctx.restore();
        },
        keydown: function (key) {
            if (key === 'Escape' || key === 'p') { switchTo('paused'); return; }
            if (key === 'ArrowLeft' || key === 'a') G.Board.moveLeft();
            else if (key === 'ArrowRight' || key === 'd') G.Board.moveRight();
            else if (key === 'ArrowDown' || key === 's') G.Board.interact();
            else if (key === 'ArrowUp' || key === 'w') G.Board.shuffleHeld();
            else if (key === ' ') G.Board.emergencyBrake();
        },
        keyup: function () {}
    };

    // ---- PAUSED ------------------------------------------------------
    var paused = {
        enter: function () { showHUD(); mgr.showOverlay('pause'); },
        exit: function () {},
        update: function () {},
        draw: function (ctx, W, H) {
            G.Board.draw(ctx, W, H, performance.now());
        },
        keydown: function (key) {
            mgr.menuNav('pause', key, function (idx, el) {
                var a = el && el.getAttribute('data-action');
                if (a === 'resume') switchTo('playing');
                else if (a === 'settings') { backTarget = 'paused'; switchTo('settings'); }
                else if (a === 'restart') { G.Board.startGame(G.Board.getMode()); switchTo('playing'); }
                else if (a === 'quit') switchTo('title');
            }, { onBack: function () { switchTo('playing'); } });
        }
    };

    // ---- GAMEOVER ----------------------------------------------------
    var gameover = {
        enter: function () {
            hideHUD();
            var st = G.Board.getStats();
            var fin = st.finished;
            // High scores
            var isHS = false;
            var entry;
            if (st.mode === 'sprint') {
                if (fin) {
                    isHS = G.Storage.qualifies('sprint', Math.floor(st.gameTime));
                    entry = { score: st.score, time: Math.floor(st.gameTime),
                              level: st.level, date: dateISO() };
                    if (isHS) G.Storage.add('sprint', entry);
                }
            } else {
                isHS = G.Storage.qualifies(st.mode, st.score);
                entry = { score: st.score, level: st.level, chain: st.bestChain,
                          time: Math.floor(st.gameTime), date: dateISO() };
                if (isHS) G.Storage.add(st.mode, entry);
            }
            var title = document.querySelector('#screen-gameover .overlay-title');
            if (title) title.textContent = fin ? (st.mode.toUpperCase() + ' COMPLETE!') : 'GAME OVER';
            if (fin) G.Audio.win();

            var el = document.getElementById('gameover-stats');
            if (el) {
                var lines = [];
                lines.push('Mode: ' + st.mode.toUpperCase());
                lines.push('Score: ' + st.score + '   Level: ' + st.level);
                lines.push('Blocks Popped: ' + st.blocksPopped);
                lines.push('Best Chain: x' + st.bestChain);
                lines.push('Time: ' + formatTime(st.gameTime));
                if (isHS) lines.push('\n★ NEW HIGH SCORE! ★');
                el.textContent = lines.join('\n');
            }
            mgr.showOverlay('gameover');
        },
        exit: function () {},
        update: function () {},
        draw: drawBg,
        keydown: function (key) {
            mgr.menuNav('gameover', key, function (idx, el) {
                var a = el && el.getAttribute('data-action');
                if (a === 'restart') { G.Board.startGame(G.Board.getMode()); switchTo('playing'); }
                else if (a === 'highscores') switchTo('highscores');
                else if (a === 'quit') switchTo('title');
            });
        }
    };

    // ---- HIGH SCORES -------------------------------------------------
    var highscores = {
        enter: function () {
            hsMode = 'classic';
            mgr.showOverlay('highscores');
            this.refresh();
        },
        exit: function () {},
        update: function () {},
        draw: drawBg,
        refresh: function () {
            var modes = ['classic', 'sprint', 'puzzle'];
            for (var i = 0; i < modes.length; i++) {
                var el = document.getElementById('hs-tab-' + modes[i]);
                if (el) el.className = modes[i] === hsMode ? 'hs-tab active' : 'hs-tab';
            }
            var list = G.Storage.list(hsMode);
            var out = document.getElementById('hs-list');
            if (!out) return;
            if (!list.length) { out.textContent = 'No scores yet'; return; }
            var lines = [];
            for (var i = 0; i < list.length; i++) {
                var s = list[i];
                var rank = (i + 1) + '.';
                if (i < 9) rank = ' ' + rank;
                if (hsMode === 'sprint') {
                    lines.push(rank + ' ' + formatTime(s.time) + '  Lv' + (s.level || 1));
                } else {
                    lines.push(rank + ' ' + (s.score || 0) + '  Lv' + (s.level || 1) +
                               '  x' + (s.chain || 1));
                }
            }
            out.textContent = lines.join('\n');
        },
        keydown: function (key) {
            var self = this;
            if (key === 'ArrowLeft' || key === 'ArrowRight') {
                var modes = ['classic', 'sprint', 'puzzle'];
                var idx = modes.indexOf(hsMode);
                if (key === 'ArrowLeft') idx = (idx - 1 + 3) % 3;
                else idx = (idx + 1) % 3;
                hsMode = modes[idx];
                self.refresh();
                G.Audio.menuMove();
                return;
            }
            mgr.menuNav('highscores', key, function () { switchTo('title'); },
                { onBack: function () { switchTo('title'); } });
        }
    };

    // ---- HOW TO PLAY -------------------------------------------------
    var howToPlay = {
        enter: function () { mgr.showOverlay('howtoplay'); },
        exit: function () {},
        update: function () {},
        draw: drawBg,
        keydown: function (key) {
            mgr.menuNav('howtoplay', key, function () { switchTo('title'); },
                { onBack: function () { switchTo('title'); } });
        }
    };

    // ---- CREDITS -----------------------------------------------------
    var credits = {
        enter: function () { mgr.showOverlay('credits'); },
        exit: function () {},
        update: function () {},
        draw: drawBg,
        keydown: function (key) {
            mgr.menuNav('credits', key, function () { switchTo('title'); },
                { onBack: function () { switchTo('title'); } });
        }
    };

    function formatTime(ms) {
        var s = Math.floor(ms / 1000);
        var m = Math.floor(s / 60);
        var sec = s % 60;
        var cs = Math.floor((ms % 1000) / 10);
        return m + ':' + (sec < 10 ? '0' : '') + sec + '.' + (cs < 10 ? '0' : '') + cs;
    }

    function dateISO() {
        try { return new Date().toISOString().slice(0, 10); }
        catch (e) { return '----'; }
    }

    return {
        init: init,
        manager: function () { return mgr; },
        switchTo: switchTo
    };
})();
