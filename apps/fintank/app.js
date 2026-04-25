// app.js — entry point + core Game module.
'use strict';
var F = window.F = window.F || {};

F.Game = (function () {
    var canvas, ctx;

    // World state
    var slot = null;          // active save slot
    var fish = [];
    var pellets = [];
    var coins = [];
    var intruders = [];
    var activePet = null;

    // Day state
    var DAY_MS = 120000; // 120 seconds
    var dayTimer = 0;
    var waveSchedule = [];
    var waveCursor = 0;
    var coinsGainedToday = 0;
    var intrudersKilledToday = 0;
    var nextStatus = 'playing';
    var lastDayBonus = 0;

    // Layout: tank interior
    var tankTop = 70, tankBottom = 760, tankLeft = 40, tankRight = 1160;

    function computeLayout(Wd, Hd) {
        tankLeft   = 30;
        tankRight  = Wd - 30;
        tankTop    = 70;
        tankBottom = Hd - 30;
    }

    // ---- Helpers ----
    function rand(a, b) { return a + Math.random() * (b - a); }

    function addPellet(x, y) {
        pellets.push({
            alive: true,
            x: x,
            y: (y != null ? y : tankTop + 10),
            vx: rand(-10, 10),
            vy: rand(20, 40),
            wobble: Math.random() * Math.PI * 2,
            age: 0
        });
        F.Particles.splash(x, tankTop);
        F.Audio.splash();
    }

    function addCoin(x, y, tier, value) {
        coins.push({
            alive: true,
            x: x, y: y,
            vx: rand(-6, 6),
            vy: rand(5, 20),
            tier: tier,
            value: value,
            age: 0,
            life: 10, // seconds
            spin: Math.random() * Math.PI * 2,
            settled: false
        });
        F.Audio.coinDrop(tier);
    }

    function collectCoin(c) {
        if (!c.alive || c._consumed) return;
        c.alive = false;
        c._consumed = true;
        slot.coins += c.value;
        slot.totalCoins = (slot.totalCoins || 0) + c.value;
        coinsGainedToday += c.value;
        F.Audio.coinGet(c.tier);
        F.Particles.floatText(c.x, c.y - 10, '+' + c.value, '#f2c95b');
    }

    function onFishAte(f) {
        F.Audio.chomp();
        F.Particles.spark(f.x, f.y, '#ffd080', 4);
    }

    function damageIntruder(iu, amt) {
        var died = F.Intruders.damage(iu, amt);
        if (died) {
            F.Audio.intruderDie();
            F.Particles.spark(iu.x, iu.y, iu.def.color, 14);
            F.Particles.shake(4);
            // drop bonus coins
            slot.coins += iu.def.reward;
            slot.totalCoins = (slot.totalCoins || 0) + iu.def.reward;
            coinsGainedToday += iu.def.reward;
            F.Particles.floatText(iu.x, iu.y, '+' + iu.def.reward, '#c0ff70');
            intrudersKilledToday++;
        } else {
            F.Audio.hit();
            F.Particles.spark(iu.x, iu.y, '#ff6060', 5);
        }
    }

    function spawnIntruder(type) {
        var def = F.Intruders.TYPES[type] || F.Intruders.TYPES.snatcher;
        var fromLeft = Math.random() < 0.5;
        var x = fromLeft ? tankLeft - 40 : tankRight + 40;
        var y = rand(tankTop + 40, tankBottom - 40);
        var iu = F.Intruders.makeIntruder(type, x, y, slot ? slot.day : 1);
        intruders.push(iu);
        F.Audio.intruderRoar();
    }

    function onEggLay(x, y) {
        // Immediately hatch a tier-1 baby fish (if capacity allows).
        if (!slot) return;
        var cap = F.Economy.maxFishCap(slot);
        if (fish.filter(function (f) { return !f.dead; }).length >= cap) return;
        var nf = F.Fish.makeFish(1, x, y);
        nf.size = 4;
        fish.push(nf);
        F.Particles.spark(x, y, '#a8f06c', 8);
        F.Audio.hatch();
    }

    // ---- Game setup ----
    function startGame(slotN) {
        slot = F.Economy.loadSlot(slotN);
        slot.slot = slotN;
        F.Economy.settings.activeSlot = slotN;
        F.Economy.saveSettings();
        // Ensure at least a couple starter fish for new slots.
        if (!slot.fish || slot.fish.length === 0) {
            slot.fish = [{ tier: 1 }, { tier: 1 }];
        }
    }

    function enterPlayScreen() {
        F.Economy.saveSlot(slot);
        beginDay();
    }

    function beginDay() {
        fish = [];
        pellets = [];
        coins = [];
        intruders = [];
        F.Particles.reset();
        // Build fish from inventory
        var Wd = getW(), Hd = getH();
        computeLayout(Wd, Hd);
        for (var i = 0; i < slot.fish.length; i++) {
            var def = F.Economy.fishById(slot.fish[i].tier) || F.Economy.FISH_TIERS[0];
            var x = rand(tankLeft + 50, tankRight - 50);
            var y = rand(tankTop + 60, tankBottom - 60);
            var f = F.Fish.makeFish(def.id, x, y);
            f.size = def.size; f.grown = true; f.hunger = 80;
            fish.push(f);
        }
        activePet = slot.activePet ? F.Pets.makeActivePet(slot.activePet) : null;

        dayTimer = 0;
        coinsGainedToday = 0;
        intrudersKilledToday = 0;
        lastDayBonus = 0;
        waveSchedule = F.Intruders.spawnWaveForDay(slot.day, Math.random);
        waveCursor = 0;
        nextStatus = 'playing';
    }

    function onEnterPlay() {
        // If called without beginDay first (e.g. from test), guarantee state
        if (!fish.length && slot) beginDay();
        refreshHUD();
    }

    // ---- Main step ----
    function tick(ms) {
        if (!slot) return;
        var Wd = getW(), Hd = getH();
        computeLayout(Wd, Hd);

        dayTimer += ms;

        // Waves
        while (waveCursor < waveSchedule.length && waveSchedule[waveCursor].t <= dayTimer) {
            spawnIntruder(waveSchedule[waveCursor].type);
            waveCursor++;
        }

        // Step fish
        var worldCtx = {
            Wd: Wd, Hd: Hd,
            tankLeft: tankLeft, tankRight: tankRight, tankTop: tankTop, tankBottom: tankBottom,
            fish: fish, pellets: pellets, coins: coins, intruders: intruders,
            pelletTier: function () { return slot.pelletTier; },
            filterMult: function () { return F.Economy.filterMult(slot); },
            onCoinDrop: function (x, y, tier, val) { addCoin(x, y, tier, val); },
            onFishAte: onFishAte,
            onEggLay: onEggLay,
            onFishEaten: function (f) { F.Audio.fishDie(); F.Particles.spark(f.x, f.y, '#ff6080', 10); },
            onCoinTaken: function (c) {
                // siphoner took the coin
                F.Particles.spark(c.x, c.y, '#ff4040', 6);
            },
            addPellet: function (x, y) { addPellet(x, y != null ? y : tankTop + 20); },
            addCoinAt: function (x, y, tier, val) { addCoin(x, y, tier, val); },
            collectCoin: collectCoin,
            damageIntruder: damageIntruder,
            onAlchemUpgrade: function (c) { F.Particles.spark(c.x, c.y, '#b06acb', 6); }
        };

        for (var i = fish.length - 1; i >= 0; i--) {
            F.Fish.step(fish[i], ms, worldCtx);
            if (fish[i]._despawn) fish.splice(i, 1);
        }

        // Step pellets
        for (var j = pellets.length - 1; j >= 0; j--) {
            var p = pellets[j];
            p.age += ms;
            p.wobble += ms / 200;
            p.x += Math.sin(p.wobble) * 12 * (ms/1000);
            p.y += p.vy * (ms/1000);
            if (!p.alive || p._consumed || p.y > tankBottom - 10 || p.age > 12000) {
                pellets.splice(j, 1);
            }
        }

        // Step coins
        for (var k = coins.length - 1; k >= 0; k--) {
            var c = coins[k];
            c.age += ms / 1000;
            c.spin += ms / 80;
            if (!c.settled) {
                c.x += c.vx * (ms/1000);
                c.y += c.vy * (ms/1000);
                c.vy += 30 * (ms/1000);
                if (c.y > tankBottom - 12) {
                    c.y = tankBottom - 12;
                    c.vy = 0; c.vx = 0; c.settled = true;
                }
            }
            if (c.age >= c.life || c._consumed) {
                coins.splice(k, 1);
            }
        }

        // Intruders
        for (var m = intruders.length - 1; m >= 0; m--) {
            F.Intruders.step(intruders[m], ms, worldCtx);
            if (intruders[m]._despawn) intruders.splice(m, 1);
        }

        // Pet
        if (activePet) F.Pets.step(activePet, ms, worldCtx);

        // Particles
        F.Particles.update(ms, Wd, Hd);

        // Prune dead fish after float animation
        for (var q = fish.length - 1; q >= 0; q--) {
            if (fish[q].dead && fish[q].y < tankTop - 10) fish.splice(q, 1);
        }

        refreshHUD();

        // Day-end / game-over
        var aliveFish = 0;
        for (var r = 0; r < fish.length; r++) if (!fish[r].dead) aliveFish++;
        if (aliveFish === 0 && slot.fish.length > 0) {
            // zero fish alive -> check if inventory also empty (no retry)
            // Actually end now
            slot.fish = []; // lose them all
            F.Economy.saveSlot(slot);
            nextStatus = 'gameover';
        }
        if (dayTimer >= DAY_MS && nextStatus === 'playing') {
            endDay();
        }
    }

    function endDay() {
        // Persist inventory from current fish
        var invNext = [];
        for (var i = 0; i < fish.length; i++) if (!fish[i].dead) invNext.push({ tier: fish[i].tier });
        slot.fish = invNext;
        // Bonus
        var bonus = invNext.length * 25 + slot.day * 20;
        slot.coins += bonus;
        slot.totalCoins = (slot.totalCoins || 0) + bonus;
        lastDayBonus = bonus;
        if (slot.day > (slot.bestDay || 0)) slot.bestDay = slot.day;
        F.Economy.saveSlot(slot);
        nextStatus = 'dayclear';
    }

    function advanceToNextDay() {
        slot.day = (slot.day || 1) + 1;
        F.Economy.saveSlot(slot);
        beginDay();
    }

    function resetSlotForRetry() {
        // Keep save slot's upgrades/pets/pelletTier but give 2 starter fish.
        slot.fish = [{ tier: 1 }, { tier: 1 }];
        slot.day = 1;
        slot.coins = Math.max(slot.coins, 150);
        F.Economy.saveSlot(slot);
    }

    // ---- Shop buys ----
    function shopBuyAt(idx) {
        var items = F.Economy.shopCatalog(slot);
        var it = items[idx];
        if (!it || it.disabled) return { ok: false };
        var res;
        if (it.kind === 'pellet') res = F.Economy.buyPelletNext(slot);
        else if (it.kind === 'fish') {
            res = F.Economy.buyFish(slot, it.id);
        }
        else if (it.kind === 'upgrade') res = F.Economy.buyUpgrade(slot, it.id);
        else if (it.kind === 'pet') res = F.Economy.buyPet(slot, it.id);
        else res = { ok: false };
        if (res.ok) F.Economy.saveSlot(slot);
        return res;
    }

    // Directly buy by kind id, used by test hook and quick-buy hotkeys.
    function buy(item) {
        if (!slot) return { ok: false };
        var res;
        if (item === 'pellet') res = F.Economy.buyPelletNext(slot);
        else if (item.indexOf('fish_tier') === 0) {
            var t = parseInt(item.substr('fish_tier'.length), 10);
            res = F.Economy.buyFish(slot, t);
            if (res.ok) {
                var Wd = getW(), Hd = getH();
                computeLayout(Wd, Hd);
                var f = F.Fish.makeFish(t, rand(tankLeft + 60, tankRight - 60), rand(tankTop + 60, tankBottom - 60));
                f.size = f.def.size * 0.6;
                fish.push(f);
            }
        }
        else if (item.indexOf('upgrade_') === 0) {
            res = F.Economy.buyUpgrade(slot, item.substr('upgrade_'.length));
        }
        else if (item.indexOf('pet_') === 0) {
            var pid = item.substr('pet_'.length);
            res = F.Economy.buyPet(slot, pid);
            if (res.ok) {
                activePet = F.Pets.makeActivePet(pid);
                slot.activePet = pid;
            }
        }
        else res = { ok: false, reason: 'UNKNOWN' };
        if (res && res.ok) F.Economy.saveSlot(slot);
        return res;
    }

    function quickBuy(n) {
        // 1=pellet, 2..6=fish tier 1..5, 5 already overloaded — keep simple:
        if (n === 1) buy('pellet');
        else if (n >= 2 && n <= 6) buy('fish_tier' + (n - 1));
    }

    function quickFeed() {
        // Drop 5 pellets spread horizontally at current mouse-ish x (center if unknown)
        var cx = (tankLeft + tankRight) / 2;
        for (var i = 0; i < 5; i++) {
            addPellet(cx + (i - 2) * 40, tankTop + 20);
        }
    }

    // ---- Input handling ----
    function clickAt(x, y) {
        // 1. Intruder hit?
        for (var i = 0; i < intruders.length; i++) {
            var iu = intruders[i];
            if (iu.dead) continue;
            if (Math.hypot(iu.x - x, iu.y - y) < iu.def.size + 6) {
                damageIntruder(iu, 1);
                return;
            }
        }
        // 2. Coin collect?
        for (var j = coins.length - 1; j >= 0; j--) {
            var c = coins[j];
            if (!c.alive || c._consumed) continue;
            if (Math.hypot(c.x - x, c.y - y) < 20) {
                collectCoin(c);
                return;
            }
        }
        // 3. Drop pellet if click is inside the tank
        if (y > tankTop + 5 && y < tankBottom - 5 && x > tankLeft && x < tankRight) {
            addPellet(x, tankTop + 15);
            F.Audio.feed();
        }
    }

    // ---- HUD ----
    function refreshHUD() {
        if (!slot) return;
        Hud.text('#hud-coins', String(slot.coins));
        Hud.text('#hud-day', String(slot.day));
        var secLeft = Math.max(0, Math.ceil((DAY_MS - dayTimer) / 1000));
        Hud.text('#hud-time', String(secLeft));
        var aliveFish = 0;
        for (var i = 0; i < fish.length; i++) if (!fish[i].dead) aliveFish++;
        Hud.text('#hud-fish', aliveFish + '/' + F.Economy.maxFishCap(slot));
        Hud.text('#hud-pet', slot.activePet ? slot.activePet.toUpperCase() : '-');
    }

    // ---- Draw ----
    function draw(dctx, Wd, Hd) {
        computeLayout(Wd, Hd);
        // Background water
        var g = dctx.createLinearGradient(0, tankTop, 0, tankBottom);
        var lightMult = slot ? F.Economy.lightMult(slot) : 1.0;
        var topCol = shadeHex('#155874', lightMult - 1);
        var botCol = shadeHex('#061a2a', 0);
        g.addColorStop(0, topCol);
        g.addColorStop(1, botCol);
        dctx.fillStyle = g;
        dctx.fillRect(0, 0, Wd, Hd);

        // Tank frame
        dctx.strokeStyle = '#2a5068';
        dctx.lineWidth = 3;
        dctx.strokeRect(tankLeft, tankTop, tankRight - tankLeft, tankBottom - tankTop);

        // Gravel
        dctx.fillStyle = '#1a2840';
        dctx.fillRect(tankLeft, tankBottom - 14, tankRight - tankLeft, 14);
        for (var gi = 0; gi < 40; gi++) {
            var gx = tankLeft + (gi / 40) * (tankRight - tankLeft) + (gi * 17 % 20);
            dctx.fillStyle = gi % 3 === 0 ? '#283b58' : '#334768';
            dctx.fillRect(gx, tankBottom - 12 + (gi % 3), 6, 4);
        }

        // Decor: two plants
        drawPlant(dctx, tankLeft + 80, tankBottom - 14, 100);
        drawPlant(dctx, tankRight - 80, tankBottom - 14, 120);

        // Particles (bubbles layer)
        F.Particles.draw(dctx);

        // Pellets
        for (var i = 0; i < pellets.length; i++) {
            var p = pellets[i];
            dctx.fillStyle = pelletColor(slot ? slot.pelletTier : 1);
            dctx.beginPath();
            dctx.arc(p.x, p.y, 4, 0, Math.PI * 2);
            dctx.fill();
            dctx.fillStyle = '#ffffff';
            dctx.globalAlpha = 0.4;
            dctx.beginPath();
            dctx.arc(p.x - 1.5, p.y - 1.5, 1.2, 0, Math.PI * 2);
            dctx.fill();
            dctx.globalAlpha = 1;
        }

        // Coins
        for (var j = 0; j < coins.length; j++) {
            drawCoin(dctx, coins[j]);
        }

        // Fish
        for (var k = 0; k < fish.length; k++) {
            F.Fish.draw(dctx, fish[k]);
        }

        // Intruders
        for (var m = 0; m < intruders.length; m++) {
            F.Intruders.draw(dctx, intruders[m]);
        }

        // Pet
        if (activePet) F.Pets.draw(dctx, activePet);
    }

    function drawCoin(dctx, c) {
        var tierColor = ['#f2c95b','#f2c95b','#ffd890','#ffec70','#fff6a0','#d8c0ff','#ffe0ff'];
        var col = tierColor[c.tier] || '#f2c95b';
        var fade = c.age > c.life - 3 ? Math.max(0.3, 1 - (c.age - (c.life - 3)) / 3) : 1.0;
        dctx.save();
        dctx.translate(c.x, c.y);
        dctx.rotate(c.spin);
        dctx.globalAlpha = fade;
        dctx.fillStyle = col;
        var sw = 10 + c.tier * 2;
        dctx.beginPath();
        dctx.ellipse(0, 0, sw, Math.abs(Math.cos(c.spin)) * sw, 0, 0, Math.PI * 2);
        dctx.fill();
        dctx.fillStyle = '#8a6020';
        dctx.fillRect(-2, -4, 4, 8);
        dctx.restore();
        dctx.globalAlpha = 1;
    }

    function drawPlant(dctx, x, yBase, h) {
        dctx.strokeStyle = '#2a6a3a';
        dctx.lineWidth = 3;
        dctx.beginPath();
        for (var i = 0; i < 5; i++) {
            var swayX = x + Math.sin(i * 0.6 + performance.now() / 800) * 8;
            var y = yBase - (i / 5) * h;
            if (i === 0) dctx.moveTo(swayX, yBase);
            dctx.lineTo(swayX + (i % 2 === 0 ? -4 : 4), y);
        }
        dctx.stroke();
    }

    function pelletColor(tier) {
        return ['#ffd08c','#ffd08c','#ffb050','#c0f070','#e0c0ff'][tier] || '#ffd08c';
    }

    function shadeHex(hex, pct) {
        var c = hex.replace('#','');
        var r = parseInt(c.substr(0,2),16);
        var g = parseInt(c.substr(2,2),16);
        var b = parseInt(c.substr(4,2),16);
        function m(x) { return Math.max(0, Math.min(255, Math.round(x + (pct > 0 ? (255-x) : x) * pct))); }
        return 'rgb(' + m(r) + ',' + m(g) + ',' + m(b) + ')';
    }

    // Status poller used by screens
    function status() { return nextStatus; }

    function dayStats() {
        return {
            day: slot.day,
            fishCount: fish.filter(function (f) { return !f.dead; }).length,
            coinsGainedToday: coinsGainedToday,
            intrudersKilled: intrudersKilledToday,
            bonus: lastDayBonus,
            coins: slot.coins
        };
    }

    function gameOverStats() {
        return { bestDay: slot.bestDay || slot.day, totalCoins: slot.totalCoins || 0, slot: slot.slot };
    }

    function getW() { return Canvas.w(ctx, 1200); }
    function getH() { return Canvas.h(ctx, 800); }

    function init() {
        canvas = document.getElementById('game');
        ctx = canvas.getContext('2d');

        F.Economy.settings; // init defaults
        F.Audio.init();
        F.Screens.init();

        // Keyboard routing
        document.body.addEventListener('keydown', function (e) {
            var name = F.Screens.manager().name();
            if (e.repeat && name === 'playing') return;
            F.Screens.manager().keydown(e.key);
        });

        // Canvas mouse routing during gameplay only.
        canvas.addEventListener('mousedown', function (e) {
            var name = F.Screens.manager().name();
            if (name !== 'playing') return;
            var r = canvas.getBoundingClientRect();
            var sx = getW() / r.width;
            var sy = getH() / r.height;
            var x = (e.clientX - (r ? r.left : 0)) * sx;
            var y = (e.clientY - (r ? r.top : 0)) * sy;
            clickAt(x, y);
        });

        F.Screens.switchTo('title');

        var loop = GameLoop.create({
            tick: function (dt) { F.Screens.manager().update(dt, getW(), getH()); },
            draw: function () {
                var Wd = getW(), Hd = getH();
                ctx.clearRect(0, 0, Wd, Hd);
                F.Screens.manager().draw(ctx, Wd, Hd);
            }
        });
        loop.start();

        return loop;
    }

    return {
        init: init,
        startGame: startGame,
        getSlot: function () { return slot; },
        enterPlayScreen: enterPlayScreen,
        onEnterPlay: onEnterPlay,
        tick: tick,
        draw: draw,
        status: status,
        dayStats: dayStats,
        gameOverStats: gameOverStats,
        refreshHUD: refreshHUD,
        shopBuyAt: shopBuyAt,
        buy: buy,
        quickBuy: quickBuy,
        quickFeed: quickFeed,
        clickAt: clickAt,
        advanceToNextDay: advanceToNextDay,
        resetSlotForRetry: resetSlotForRetry,

        // Direct accessors for test hooks
        _state: function () {
            return {
                slot: slot, fish: fish, pellets: pellets, coins: coins,
                intruders: intruders, pet: activePet, dayTimer: dayTimer
            };
        },
        _forceEndDay: function () { endDay(); },
        _addCoins: function (n) { slot.coins += n; slot.totalCoins = (slot.totalCoins||0) + n; F.Economy.saveSlot(slot); },
        _addPellet: addPellet,
        _addCoin: addCoin,
        _spawnIntruder: spawnIntruder,
        _killAllIntruders: function () {
            for (var i = 0; i < intruders.length; i++) {
                if (!intruders[i].dead) {
                    intruders[i].dead = true;
                    intrudersKilledToday++;
                }
            }
        },
        _collectAllCoins: function () {
            for (var i = 0; i < coins.length; i++) {
                if (coins[i].alive && !coins[i]._consumed) collectCoin(coins[i]);
            }
        },
        _feedFish: function (index) {
            // Force one fish to be fed: jumpstart coin drop
            var f = fish[index || 0];
            if (!f) return;
            f.hasFood = true;
            f.coinTimer = 100; // drop in ~100ms
            f.hunger = 100;
        }
    };
})();

// ---- Entry ----
(function () {
    var loop = F.Game.init();

    // Expose test hooks
    window.__fintank = {
        F: F,
        state: function () { return F.Game._state(); },
        feed: function (x) { F.Game._addPellet(x != null ? x : 600, 100); },
        buy: function (item) { return F.Game.buy(item); },
        addCoins: function (n) { F.Game._addCoins(n); },
        spawnIntruder: function (type) { F.Game._spawnIntruder(type || 'snatcher'); },
        killAllIntruders: function () { F.Game._killAllIntruders(); },
        collectAllCoins: function () { F.Game._collectAllCoins(); },
        feedFish: function (i) { F.Game._feedFish(i); },
        endDay: function () { F.Game._forceEndDay(); },
        dayProgress: function () { var s = F.Game._state(); return s.dayTimer; },
        screens: F.Screens,
        game: F.Game,
        economy: F.Economy,
        loop: loop
    };

    console.log('Fintank loaded.');
})();
