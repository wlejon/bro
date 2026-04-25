// screens.js — screen state machine.
'use strict';
var F = window.F = window.F || {};

F.Screens = (function () {
    var mgr = null;
    var backTarget = 'title';
    var bgT = 0;
    var shopSel = 0;

    function init() {
        mgr = Screens.create({
            overlay: '#overlay',
            prefix: 'screen-',
            onMenuMove: function () { F.Audio.menuMove(); },
            onMenuSelect: function () { F.Audio.menuSelect(); }
        });
        mgr.define('title',      titleScreen);
        mgr.define('slots',      slotsScreen);
        mgr.define('shop',       shopScreen);
        mgr.define('playing',    playing);
        mgr.define('paused',     paused);
        mgr.define('dayclear',   dayClear);
        mgr.define('gameover',   gameover);
        mgr.define('highscores', highscores);
        mgr.define('howtoplay',  howToPlay);
        mgr.define('credits',    credits);
        mgr.define('settings',   settingsScreen);
    }

    function manager() { return mgr; }
    function switchTo(n, p) { mgr.switchTo(n, p); }

    // ---- Shared water background ----
    function drawBg(ctx, Wd, Hd) {
        bgT += 16;
        var g = ctx.createLinearGradient(0, 0, 0, Hd);
        g.addColorStop(0, '#08283a');
        g.addColorStop(1, '#020e1a');
        ctx.fillStyle = g;
        ctx.fillRect(0, 0, Wd, Hd);

        // ambient wavy lines
        for (var i = 0; i < 6; i++) {
            ctx.globalAlpha = 0.04 + (i % 2) * 0.02;
            ctx.strokeStyle = '#5fc0ff';
            ctx.lineWidth = 1;
            ctx.beginPath();
            var y = (i * 120 + (bgT * 0.02) % 120);
            for (var x = 0; x <= Wd; x += 20) {
                var yy = y + Math.sin((x + bgT * 0.01 + i * 30) * 0.03) * 6;
                if (x === 0) ctx.moveTo(x, yy);
                else ctx.lineTo(x, yy);
            }
            ctx.stroke();
        }
        ctx.globalAlpha = 1;
    }

    function selIdx(screenId) {
        var items = mgr.getMenuItems(screenId);
        for (var i = 0; i < items.length; i++) if (items[i].classList.contains('selected')) return i;
        return 0;
    }

    function showHUD() { Hud.show('#hud'); }
    function hideHUD() { Hud.hide('#hud'); }

    // ---- TITLE ----
    var titleScreen = {
        enter: function () { mgr.showOverlay('title'); hideHUD(); },
        exit: function () {},
        update: function () {},
        draw: drawBg,
        keydown: function (key) {
            mgr.menuNav('title', key, function (idx, el) {
                var a = el && el.getAttribute('data-action');
                if (a === 'play') switchTo('slots');
                else if (a === 'highscores') switchTo('highscores');
                else if (a === 'howtoplay') switchTo('howtoplay');
                else if (a === 'settings') { backTarget = 'title'; switchTo('settings'); }
                else if (a === 'credits') switchTo('credits');
                else if (a === 'quit') { try { window.close(); } catch (e) {} }
            });
        }
    };

    // ---- SLOTS ----
    var slotsScreen = {
        enter: function () { mgr.showOverlay('slots'); this.refresh(); },
        exit: function () {},
        update: function () {},
        draw: drawBg,
        refresh: function () {
            var items = mgr.getMenuItems('slots');
            for (var i = 0; i < items.length; i++) {
                var slot = items[i].getAttribute('data-slot');
                if (!slot) continue;
                var n = parseInt(slot, 10);
                var s = F.Economy.loadSlot(n);
                var fresh = (s.day === 1 && s.fish.length === 0 && s.coins === 200 && !s.bestDay);
                items[i].textContent = 'SLOT ' + n + (fresh ? ' - NEW'
                    : ' - DAY ' + s.day + ' - ' + s.coins + 'C - ' + s.fish.length + ' FISH');
            }
        },
        keydown: function (key) {
            var self = this;
            if (key === 'Delete' || key === 'Backspace') {
                var items = mgr.getMenuItems('slots');
                var idx = selIdx('slots');
                var el = items[idx];
                var s = el && el.getAttribute('data-slot');
                if (s) { F.Economy.eraseSlot(parseInt(s, 10)); self.refresh(); }
                return;
            }
            mgr.menuNav('slots', key, function (idx, el) {
                var s = el && el.getAttribute('data-slot');
                var a = el && el.getAttribute('data-action');
                if (s) { F.Game.startGame(parseInt(s, 10)); switchTo('shop'); }
                else if (a === 'back') switchTo('title');
            }, { onBack: function () { switchTo('title'); } });
        }
    };

    // ---- SHOP ----
    var shopScreen = {
        enter: function () {
            mgr.showOverlay('shop');
            showHUD();
            F.Game.refreshHUD();
            shopSel = 0;
            this.rebuild();
        },
        exit: function () {},
        update: function () { F.Game.refreshHUD(); },
        draw: drawBg,
        rebuild: function () {
            var slot = F.Game.getSlot();
            var items = F.Economy.shopCatalog(slot);
            var host = document.getElementById('shop-items');
            if (!host) return;
            host.innerHTML = '';
            for (var i = 0; i < items.length; i++) {
                var it = items[i];
                var div = document.createElement('div');
                div.className = 'menu-item';
                if (i === 0) div.classList.add('selected');
                if (it.disabled) div.classList.add('disabled');
                div.setAttribute('data-shop-idx', String(i));
                var priceStr = it.price < 0 ? '' : ('  ' + it.price + 'C');
                div.textContent = it.label + priceStr;
                host.appendChild(div);
            }
            // Add next day button
            var next = document.createElement('div');
            next.className = 'menu-item';
            next.setAttribute('data-action', 'next');
            next.textContent = '>> START DAY ' + slot.day;
            host.appendChild(next);

            var title = document.getElementById('shop-subtitle');
            if (title) title.textContent = 'DAY ' + slot.day + ' - ' + slot.coins + ' COINS';
        },
        keydown: function (key) {
            var self = this;
            mgr.menuNav('shop', key, function (idx, el) {
                var a = el && el.getAttribute('data-action');
                if (a === 'next') {
                    F.Game.enterPlayScreen();
                    switchTo('playing');
                    return;
                }
                var shopIdx = el && el.getAttribute('data-shop-idx');
                if (shopIdx != null) {
                    var i = parseInt(shopIdx, 10);
                    var res = F.Game.shopBuyAt(i);
                    if (res && res.ok) { F.Audio.buy(); }
                    else { F.Audio.buyFail(); }
                    self.rebuild();
                }
            }, {
                onBack: function () { switchTo('dayclear'); }
            });
            if (key === 'n' || key === 'N') {
                F.Game.enterPlayScreen();
                switchTo('playing');
            }
        }
    };

    // ---- PLAYING ----
    var playing = {
        enter: function () {
            mgr.hideOverlay();
            showHUD();
            F.Game.onEnterPlay();
        },
        exit: function () { hideHUD(); },
        update: function (dt) {
            F.Game.tick(dt);
            var st = F.Game.status();
            if (st === 'dayclear') switchTo('dayclear');
            else if (st === 'gameover') switchTo('gameover');
        },
        draw: function (ctx, Wd, Hd) {
            var sh = F.Particles.shakeOffset();
            ctx.save();
            ctx.translate(sh.x, sh.y);
            F.Game.draw(ctx, Wd, Hd);
            ctx.restore();
        },
        keydown: function (key) {
            if (key === 'Escape' || key === 'p' || key === 'P') { switchTo('paused'); return; }
            if (key === ' ') { F.Game.quickFeed(); return; }
            // Number keys quick-buy
            if (/^[1-5]$/.test(key)) {
                F.Game.quickBuy(parseInt(key, 10));
            }
        },
        mousedown: function (x, y) { F.Game.clickAt(x, y); }
    };

    // ---- PAUSED ----
    var paused = {
        enter: function () { mgr.showOverlay('pause'); },
        exit: function () {},
        update: function () {},
        draw: function (ctx, Wd, Hd) {
            F.Game.draw(ctx, Wd, Hd);
        },
        keydown: function (key) {
            mgr.menuNav('pause', key, function (idx, el) {
                var a = el && el.getAttribute('data-action');
                if (a === 'resume') switchTo('playing');
                else if (a === 'settings') { backTarget = 'paused'; switchTo('settings'); }
                else if (a === 'quit') switchTo('title');
            }, { onBack: function () { switchTo('playing'); } });
        }
    };

    // ---- DAY CLEAR ----
    var dayClear = {
        enter: function () {
            mgr.showOverlay('dayclear');
            F.Audio.dayEnd();
            var st = F.Game.dayStats();
            var el = document.getElementById('dayclear-stats');
            if (el) {
                var lines = [];
                lines.push('DAY ' + st.day + ' SURVIVED');
                lines.push('FISH REMAINING: ' + st.fishCount);
                lines.push('COINS COLLECTED: ' + st.coinsGainedToday);
                lines.push('INTRUDERS DEFEATED: ' + st.intrudersKilled);
                lines.push('BONUS: +' + st.bonus + 'C');
                lines.push('BALANCE: ' + st.coins + 'C');
                el.textContent = lines.join('\n');
            }
        },
        exit: function () {},
        update: function () {},
        draw: drawBg,
        keydown: function (key) {
            mgr.menuNav('dayclear', key, function (idx, el) {
                var a = el && el.getAttribute('data-action');
                if (a === 'shop') { F.Game.advanceToNextDay(); switchTo('shop'); }
                else if (a === 'next') { F.Game.advanceToNextDay(); F.Game.enterPlayScreen(); switchTo('playing'); }
                else if (a === 'quit') switchTo('title');
            });
        }
    };

    // ---- GAME OVER ----
    var gameover = {
        enter: function () {
            mgr.showOverlay('gameover');
            F.Audio.gameover();
            var st = F.Game.gameOverStats();
            F.Economy.addHS({ day: st.bestDay, slot: st.slot, totalCoins: st.totalCoins });
            var el = document.getElementById('gameover-stats');
            if (el) {
                var lines = [];
                lines.push('ALL YOUR FISH WERE LOST');
                lines.push('BEST DAY: ' + st.bestDay);
                lines.push('TOTAL COINS: ' + st.totalCoins);
                lines.push('SLOT: ' + st.slot);
                el.textContent = lines.join('\n');
            }
        },
        exit: function () {},
        update: function () {},
        draw: drawBg,
        keydown: function (key) {
            mgr.menuNav('gameover', key, function (idx, el) {
                var a = el && el.getAttribute('data-action');
                if (a === 'restart') { F.Game.resetSlotForRetry(); switchTo('shop'); }
                else if (a === 'highscores') switchTo('highscores');
                else if (a === 'quit') switchTo('title');
            });
        }
    };

    // ---- HIGH SCORES ----
    var highscores = {
        enter: function () { mgr.showOverlay('highscores'); this.refresh(); },
        exit: function () {},
        update: function () {},
        draw: drawBg,
        refresh: function () {
            var out = document.getElementById('hs-list');
            if (!out) return;
            var list = F.Economy.listHS();
            if (!list.length) { out.textContent = 'NO SCORES YET'; return; }
            var lines = list.map(function (e, i) {
                var rank = (i + 1) + '.';
                if (i < 9) rank = ' ' + rank;
                return rank + ' DAY ' + (e.day || 0) + '  COINS ' + (e.totalCoins || 0) + '  SLOT ' + (e.slot || '?');
            });
            out.textContent = lines.join('\n');
        },
        keydown: function (key) {
            mgr.menuNav('highscores', key, function () { switchTo('title'); },
                { onBack: function () { switchTo('title'); } });
        }
    };

    // ---- HOW TO PLAY ----
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

    // ---- CREDITS ----
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

    // ---- SETTINGS ----
    var settingsScreen = {
        enter: function () { mgr.showOverlay('settings'); this.refresh(); },
        exit: function () {},
        update: function () {},
        draw: drawBg,
        refresh: function () {
            var s = F.Economy.settings;
            Hud.text('#opt-sfxVol', String(s.sfxVol));
            Hud.text('#opt-musicVol', String(s.musicVol));
            Hud.text('#opt-difficulty', F.Economy.difficultyLabel());
        },
        adjust: function (dir) {
            var items = mgr.getMenuItems('settings');
            var idx = selIdx('settings');
            var item = items[idx]; if (!item) return;
            var k = item.getAttribute('data-setting'); if (!k) return;
            var s = F.Economy.settings;
            if (k === 'sfxVol') {
                s.sfxVol = Math.max(0, Math.min(100, s.sfxVol + dir * 10));
                F.Audio.setSfxVol(s.sfxVol / 100);
            } else if (k === 'musicVol') {
                s.musicVol = Math.max(0, Math.min(100, s.musicVol + dir * 10));
                F.Audio.setMusicVol(s.musicVol / 100);
            } else if (k === 'difficulty') {
                s.difficulty = Math.max(0, Math.min(2, s.difficulty + dir));
            }
            F.Economy.saveSettings();
            this.refresh();
            F.Audio.menuMove();
        },
        keydown: function (key) {
            var self = this;
            mgr.menuNav('settings', key, function (idx, el) {
                if (el && el.getAttribute('data-action') === 'back') switchTo(backTarget);
                else self.adjust(1);
            }, {
                onAdjust: function (d) { self.adjust(d); },
                onBack: function () { switchTo(backTarget); }
            });
        }
    };

    return {
        init: init,
        manager: function () { return mgr; },
        switchTo: switchTo
    };
})();
