// screens.js — screen state machine using apps/lib Screens.
'use strict';
var W = window.W = window.W || {};

W.Screens = (function () {
    var mgr = null;
    var hsTab = 'classic';
    var backTarget = 'title';
    var bgT = 0;

    function init() {
        mgr = Screens.create({
            overlay: '#overlay',
            prefix: 'screen-',
            onMenuMove: function () { W.Audio.menuMove(); },
            onMenuSelect: function () { W.Audio.menuSelect(); }
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
        mgr.define('loading',    loadingScreen);
    }

    function manager() { return mgr; }
    function switchTo(n, payload) { mgr.switchTo(n, payload); }

    // ---- Shared background ----
    function drawBg(ctx, Wd, Hd) {
        bgT += 16;
        var g = ctx.createLinearGradient(0, 0, 0, Hd);
        g.addColorStop(0, '#120a24');
        g.addColorStop(1, '#050210');
        ctx.fillStyle = g;
        ctx.fillRect(0, 0, Wd, Hd);

        // Floating pixel-font glyphs as ambient backdrop.
        var glyphs = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ';
        for (var i = 0; i < 22; i++) {
            var x = ((i * 97 + bgT * 0.04) % (Wd + 120)) - 60;
            var y = ((i * 151 + bgT * 0.03) % (Hd + 60));
            ctx.globalAlpha = 0.05 + (i % 5) * 0.015;
            var col = i % 3 === 0 ? '#e8c168' : (i % 3 === 1 ? '#8cdff6' : '#c8b8e8');
            W.Text.drawCentered(ctx, glyphs.charAt(i % glyphs.length),
                                     Math.floor(x), Math.floor(y), 6, col);
        }
        ctx.globalAlpha = 1.0;
    }

    function showHUD() { Hud.show('#hud'); }
    function hideHUD() { Hud.hide('#hud'); }

    // ---- LOADING ----
    var loadingScreen = {
        enter: function () { mgr.showOverlay('loading'); hideHUD(); },
        exit:  function () {},
        update: function () {},
        draw:   drawBg,
        keydown: function () {}
    };

    // ---- TITLE ----
    var titleScreen = {
        enter: function () { mgr.showOverlay('title'); hideHUD(); },
        exit:  function () {},
        update: function () {},
        draw: drawBg,
        keydown: function (key) {
            mgr.menuNav('title', key, function (idx, el) {
                var a = el && el.getAttribute('data-action');
                if (a === 'play') switchTo('modeSelect');
                else if (a === 'highscores') switchTo('highscores');
                else if (a === 'howtoplay') switchTo('howtoplay');
                else if (a === 'settings') { backTarget = 'title'; switchTo('settings'); }
                else if (a === 'credits') switchTo('credits');
                else if (a === 'quit') { try { window.close(); } catch (e) {} }
            });
        }
    };

    // ---- MODE SELECT ----
    var modeSelect = {
        enter: function () { mgr.showOverlay('mode-select'); hideHUD(); },
        exit:  function () {},
        update: function () {},
        draw: drawBg,
        keydown: function (key) {
            mgr.menuNav('mode-select', key, function (idx, el) {
                var m = el && el.getAttribute('data-mode');
                var a = el && el.getAttribute('data-action');
                if (m) {
                    W.Board.startGame(m);
                    switchTo('playing');
                } else if (a === 'back') switchTo('title');
            }, { onBack: function () { switchTo('title'); } });
        }
    };

    // ---- SETTINGS ----
    var settingsScreen = {
        enter: function () { mgr.showOverlay('settings'); this.refresh(); },
        exit:  function () {},
        update: function () {},
        draw:   drawBg,
        refresh: function () {
            var s = W.Storage.settings;
            Hud.text('#opt-sfxVol',   String(s.sfxVol));
            Hud.text('#opt-musicVol', String(s.musicVol));
            Hud.text('#opt-difficulty', W.Storage.difficultyLabel());
        },
        adjust: function (dir) {
            var items = mgr.getMenuItems('settings');
            var idx = 0;
            for (var i = 0; i < items.length; i++) if (items[i].classList.contains('selected')) { idx = i; break; }
            var item = items[idx]; if (!item) return;
            var k = item.getAttribute('data-setting'); if (!k) return;
            var s = W.Storage.settings;
            if (k === 'sfxVol') {
                s.sfxVol = Math.max(0, Math.min(100, s.sfxVol + dir * 10));
                W.Audio.setSfxVol(s.sfxVol / 100);
            } else if (k === 'musicVol') {
                s.musicVol = Math.max(0, Math.min(100, s.musicVol + dir * 10));
                W.Audio.setMusicVol(s.musicVol / 100);
            } else if (k === 'difficulty') {
                s.difficulty = Math.max(0, Math.min(2, s.difficulty + dir));
            }
            W.Storage.save();
            this.refresh();
            W.Audio.menuMove();
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

    // ---- PLAYING ----
    var playing = {
        enter: function () {
            mgr.hideOverlay();
            showHUD();
            W.Board.showTileLayer(true);
            W.Board.updateHUD();
        },
        exit: function () { hideHUD(); W.Board.showTileLayer(false); },
        update: function (dt) {
            W.Board.tick(dt);
            if (W.Board.isGameOver() || W.Board.isFinished()) {
                switchTo('gameover');
            }
        },
        draw: function (ctx, Wd, Hd) {
            drawBg(ctx, Wd, Hd);
            var sh = W.Particles.shakeOffset();
            ctx.save();
            ctx.translate(sh.x, sh.y);
            W.Board.draw(ctx, Wd, Hd, performance.now());
            ctx.restore();
        },
        keydown: function (key) {
            if (key === 'Escape' || key === 'p') { switchTo('paused'); return; }
            if (key === 'ArrowLeft')       W.Board.moveCursor(-1, 0);
            else if (key === 'ArrowRight') W.Board.moveCursor(1, 0);
            else if (key === 'ArrowUp')    W.Board.moveCursor(0, -1);
            else if (key === 'ArrowDown')  W.Board.moveCursor(0, 1);
            else if (key === ' ')          W.Board.keyAddAtCursor();
            else if (key === 'Enter')      W.Board.submitChain();
            else if (key === 'Backspace')  W.Board.removeLastTile();
        },
        keyup: function () {}
    };

    // ---- PAUSED ----
    var paused = {
        enter: function () { showHUD(); W.Board.showTileLayer(false); mgr.showOverlay('pause'); },
        exit:  function () {},
        update: function () {},
        draw: function (ctx, Wd, Hd) {
            drawBg(ctx, Wd, Hd);
            W.Board.draw(ctx, Wd, Hd, performance.now());
        },
        keydown: function (key) {
            mgr.menuNav('pause', key, function (idx, el) {
                var a = el && el.getAttribute('data-action');
                if (a === 'resume') switchTo('playing');
                else if (a === 'settings') { backTarget = 'paused'; switchTo('settings'); }
                else if (a === 'restart') { W.Board.startGame(W.Board.getMode()); switchTo('playing'); }
                else if (a === 'quit') switchTo('title');
            }, { onBack: function () { switchTo('playing'); } });
        }
    };

    // ---- GAME OVER ----
    var gameover = {
        enter: function () {
            hideHUD();
            var st = W.Board.getStats();
            // Determine if it qualifies for high-score lists.
            var isHS = W.Storage.qualifies(st.mode, st.score);
            var entry = {
                score: st.score, words: st.words, longest: st.longest,
                best: st.bestWord, bestScore: st.bestWordScore,
                time: Math.floor(st.gameTime),
                date: dateISO()
            };
            if (isHS) W.Storage.add(st.mode, entry);

            var title = document.querySelector('#screen-gameover .overlay-title');
            if (title) title.textContent = st.finished ? (st.mode.toUpperCase() + ' COMPLETE!') : 'GAME OVER';
            if (st.finished) W.Audio.win();

            var el = document.getElementById('gameover-stats');
            if (el) {
                var lines = [];
                lines.push('Mode: ' + st.mode.toUpperCase());
                lines.push('Score: ' + st.score);
                lines.push('Words Played: ' + st.words);
                lines.push('Longest: ' + (st.longest ? st.longest.toUpperCase() : '-'));
                lines.push('Best: ' + (st.bestWord ? (st.bestWord.toUpperCase() + ' +' + st.bestWordScore) : '-'));
                lines.push('Time: ' + formatTime(st.gameTime));
                if (isHS) lines.push('\n* NEW HIGH SCORE *');
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
                if (a === 'restart') { W.Board.startGame(W.Board.getMode()); switchTo('playing'); }
                else if (a === 'highscores') switchTo('highscores');
                else if (a === 'quit') switchTo('title');
            });
        }
    };

    // ---- HIGH SCORES ----
    var highscores = {
        enter: function () { hsTab = 'classic'; mgr.showOverlay('highscores'); this.refresh(); },
        exit:  function () {},
        update: function () {},
        draw:   drawBg,
        refresh: function () {
            var tabs = ['classic', 'timed', 'puzzle', 'words'];
            for (var i = 0; i < tabs.length; i++) {
                var t = document.getElementById('hs-tab-' + tabs[i]);
                if (t) t.className = (tabs[i] === hsTab) ? 'hs-tab active' : 'hs-tab';
            }
            var out = document.getElementById('hs-list');
            if (!out) return;
            var lines;
            if (hsTab === 'words') {
                var tw = W.Storage.topWords();
                if (!tw.length) { out.textContent = 'No words yet'; return; }
                lines = tw.map(function (e, i) {
                    var rank = (i + 1) + '.';
                    if (i < 9) rank = ' ' + rank;
                    return rank + ' ' + (e.word || '').toUpperCase() +
                           '  +' + (e.score || 0) + ' (' + (e.mode || '?') + ')';
                });
            } else {
                var list = W.Storage.list(hsTab);
                if (!list.length) { out.textContent = 'No scores yet'; return; }
                lines = list.map(function (e, i) {
                    var rank = (i + 1) + '.';
                    if (i < 9) rank = ' ' + rank;
                    return rank + ' ' + (e.score || 0) +
                           '  Words:' + (e.words || 0) +
                           '  Best:' + ((e.best || '-').toUpperCase());
                });
            }
            out.textContent = lines.join('\n');
        },
        keydown: function (key) {
            var self = this;
            if (key === 'ArrowLeft' || key === 'ArrowRight') {
                var tabs = ['classic', 'timed', 'puzzle', 'words'];
                var idx = tabs.indexOf(hsTab);
                if (key === 'ArrowLeft') idx = (idx - 1 + tabs.length) % tabs.length;
                else idx = (idx + 1) % tabs.length;
                hsTab = tabs[idx];
                self.refresh();
                W.Audio.menuMove();
                return;
            }
            mgr.menuNav('highscores', key, function () { switchTo('title'); },
                { onBack: function () { switchTo('title'); } });
        }
    };

    // ---- HOW TO PLAY ----
    var howToPlay = {
        enter: function () { mgr.showOverlay('howtoplay'); },
        exit:  function () {},
        update: function () {},
        draw:   drawBg,
        keydown: function (key) {
            mgr.menuNav('howtoplay', key, function () { switchTo('title'); },
                { onBack: function () { switchTo('title'); } });
        }
    };

    // ---- CREDITS ----
    var credits = {
        enter: function () { mgr.showOverlay('credits'); },
        exit:  function () {},
        update: function () {},
        draw:   drawBg,
        keydown: function (key) {
            mgr.menuNav('credits', key, function () { switchTo('title'); },
                { onBack: function () { switchTo('title'); } });
        }
    };

    function formatTime(ms) {
        var s = Math.floor(ms / 1000);
        var m = Math.floor(s / 60);
        var sec = s % 60;
        return m + ':' + (sec < 10 ? '0' : '') + sec;
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
