// intruders.js — original intruder types (snatcher, siphoner, drifter, swarmer, boss)
'use strict';
var F = window.F = window.F || {};

F.Intruders = (function () {
    // type definitions (original, not trademarked):
    //   snatcher  — small fast fish predator
    //   siphoner  — drains coins
    //   drifter   — slow tanky, eats multiple
    //   swarmer   — weak, spawns in groups
    //   wrecker   — boss, spawns day % 5 == 0
    var TYPES = {
        snatcher:  { hp: 2, dmgClick: 1, speed: 70,  size: 18, color: '#b06acb', eats: 'fish',   reward: 20 },
        siphoner:  { hp: 3, dmgClick: 1, speed: 55,  size: 22, color: '#d85050', eats: 'coins',  reward: 30 },
        drifter:   { hp: 6, dmgClick: 1, speed: 30,  size: 30, color: '#5a7080', eats: 'fish',   reward: 60 },
        swarmer:   { hp: 1, dmgClick: 1, speed: 90,  size: 14, color: '#c0ff70', eats: 'fish',   reward: 10 },
        wrecker:   { hp: 25, dmgClick: 1, speed: 35, size: 44, color: '#903030', eats: 'all',    reward: 300 }
    };

    function makeIntruder(type, x, y, dayScale) {
        var t = TYPES[type] || TYPES.snatcher;
        dayScale = dayScale || 1;
        return {
            type: type,
            def: t,
            x: x, y: y,
            vx: (x < 0 ? 1 : -1) * t.speed,
            vy: (Math.random() - 0.5) * 15,
            hp: Math.ceil(t.hp * (1 + (dayScale - 1) * 0.2)),
            hpMax: Math.ceil(t.hp * (1 + (dayScale - 1) * 0.2)),
            targetFish: null,
            eatCooldown: 0,
            wag: 0,
            dead: false,
            dying: 0, // seconds counting up when dead to animate
            hitFlash: 0,
            entered: false
        };
    }

    // ctx: Wd, Hd, tankLeft/Right/Top/Bottom, fish, coins, onFishEaten(f), onCoinTaken(c),
    //      onIntruderDie(i)
    function step(iu, ms, ctx) {
        if (iu.dead) {
            iu.dying += ms;
            iu.y += 0.02 * ms;
            if (iu.dying > 800) iu._despawn = true;
            return;
        }
        iu.wag += ms / 100;
        if (iu.hitFlash > 0) iu.hitFlash = Math.max(0, iu.hitFlash - ms);
        if (iu.eatCooldown > 0) iu.eatCooldown = Math.max(0, iu.eatCooldown - ms);

        // Enter the tank
        if (!iu.entered) {
            iu.x += iu.vx * (ms/1000);
            if (iu.x > ctx.tankLeft + 10 && iu.x < ctx.tankRight - 10) iu.entered = true;
            return;
        }

        var def = iu.def;
        // Find target
        if (!iu.targetFish) {
            if (def.eats === 'fish' || def.eats === 'all') {
                iu.targetFish = nearestFish(iu, ctx.fish);
            }
        }
        if (iu.targetFish && iu.targetFish.dead) iu.targetFish = null;

        if (def.eats === 'coins') {
            var c = nearestCoin(iu, ctx.coins);
            if (c) {
                moveTowards(iu, c.x, c.y, ms);
                if (dist(iu, c) < 16) {
                    c._consumed = true;
                    if (ctx.onCoinTaken) ctx.onCoinTaken(c);
                }
            } else {
                wander(iu, ms);
            }
        } else if (iu.targetFish) {
            moveTowards(iu, iu.targetFish.x, iu.targetFish.y, ms);
            if (dist(iu, iu.targetFish) < 18 && iu.eatCooldown <= 0) {
                iu.targetFish.dead = true;
                iu.eatCooldown = 1500;
                if (ctx.onFishEaten) ctx.onFishEaten(iu.targetFish);
                iu.targetFish = null;
            }
        } else {
            wander(iu, ms);
        }

        // Clamp
        if (iu.x < ctx.tankLeft + 10) { iu.x = ctx.tankLeft + 10; iu.vx = Math.abs(iu.vx); }
        if (iu.x > ctx.tankRight - 10) { iu.x = ctx.tankRight - 10; iu.vx = -Math.abs(iu.vx); }
        if (iu.y < ctx.tankTop + 20) { iu.y = ctx.tankTop + 20; iu.vy = Math.abs(iu.vy); }
        if (iu.y > ctx.tankBottom - 20) { iu.y = ctx.tankBottom - 20; iu.vy = -Math.abs(iu.vy); }
    }

    function wander(iu, ms) {
        iu.x += iu.vx * (ms/1000);
        iu.y += iu.vy * (ms/1000);
        iu.vy += (Math.random() - 0.5) * 12;
        iu.vy *= 0.96;
    }

    function moveTowards(iu, tx, ty, ms) {
        var dx = tx - iu.x, dy = ty - iu.y;
        var d = Math.hypot(dx, dy) || 1;
        var spd = iu.def.speed;
        iu.vx = dx / d * spd;
        iu.vy = dy / d * spd;
        iu.x += iu.vx * (ms/1000);
        iu.y += iu.vy * (ms/1000);
    }

    function dist(a, b) { return Math.hypot(a.x - b.x, a.y - b.y); }

    function nearestFish(iu, fish) {
        var best = null, bd = 99999;
        for (var i = 0; i < fish.length; i++) {
            var f = fish[i];
            if (f.dead) continue;
            var d = dist(iu, f);
            if (d < bd) { bd = d; best = f; }
        }
        return best;
    }
    function nearestCoin(iu, coins) {
        var best = null, bd = 99999;
        for (var i = 0; i < coins.length; i++) {
            var c = coins[i];
            if (!c.alive || c._consumed) continue;
            var d = dist(iu, c);
            if (d < bd) { bd = d; best = c; }
        }
        return best;
    }

    function damage(iu, amt) {
        if (iu.dead) return false;
        iu.hp -= amt;
        iu.hitFlash = 120;
        if (iu.hp <= 0) { iu.dead = true; return true; }
        return false;
    }

    function draw(ctx, iu) {
        ctx.save();
        ctx.translate(iu.x, iu.y);
        var facing = iu.vx < 0 ? -1 : 1;
        ctx.scale(facing, 1);
        var sz = iu.def.size;
        if (iu.hitFlash > 0) ctx.globalAlpha = 0.6 + 0.4 * Math.sin(iu.hitFlash * 0.2);

        if (iu.type === 'swarmer') {
            // Spiky triangle
            ctx.fillStyle = iu.def.color;
            ctx.beginPath();
            ctx.moveTo(sz, 0);
            ctx.lineTo(-sz, -sz * 0.7);
            ctx.lineTo(-sz * 0.7, 0);
            ctx.lineTo(-sz, sz * 0.7);
            ctx.closePath();
            ctx.fill();
        } else if (iu.type === 'drifter') {
            // Blobby
            ctx.fillStyle = iu.def.color;
            ctx.beginPath();
            ctx.ellipse(0, 0, sz, sz * 0.8, 0, 0, Math.PI * 2);
            ctx.fill();
            // spots
            ctx.fillStyle = F.Fish.shade(iu.def.color, -0.3);
            for (var i = -1; i <= 1; i++) {
                ctx.beginPath();
                ctx.arc(i * sz * 0.4, -sz * 0.15, sz * 0.15, 0, Math.PI * 2);
                ctx.fill();
            }
        } else if (iu.type === 'siphoner') {
            // Mouth-dominant red fish
            ctx.fillStyle = iu.def.color;
            ctx.beginPath();
            ctx.ellipse(0, 0, sz, sz * 0.65, 0, 0, Math.PI * 2);
            ctx.fill();
            // Big jagged mouth
            ctx.fillStyle = '#300808';
            ctx.beginPath();
            ctx.moveTo(sz, 0);
            ctx.lineTo(sz * 0.4, -sz * 0.4);
            ctx.lineTo(sz * 0.6, 0);
            ctx.lineTo(sz * 0.4, sz * 0.4);
            ctx.closePath();
            ctx.fill();
        } else if (iu.type === 'wrecker') {
            // Big boss
            ctx.fillStyle = iu.def.color;
            ctx.beginPath();
            ctx.ellipse(0, 0, sz * 1.2, sz * 0.8, 0, 0, Math.PI * 2);
            ctx.fill();
            // spikes
            ctx.fillStyle = '#402020';
            for (var k = 0; k < 5; k++) {
                var a = (k - 2) * 0.35;
                ctx.save();
                ctx.rotate(a);
                ctx.beginPath();
                ctx.moveTo(0, -sz * 0.8);
                ctx.lineTo(-6, -sz * 1.2);
                ctx.lineTo(6, -sz * 1.2);
                ctx.closePath();
                ctx.fill();
                ctx.restore();
            }
            // red eye
            ctx.fillStyle = '#ffa040';
            ctx.beginPath();
            ctx.arc(sz * 0.6, -sz * 0.2, sz * 0.18, 0, Math.PI * 2);
            ctx.fill();
        } else {
            // snatcher default — sleek predator
            ctx.fillStyle = iu.def.color;
            ctx.beginPath();
            ctx.ellipse(0, 0, sz, sz * 0.4, 0, 0, Math.PI * 2);
            ctx.fill();
            // tail
            ctx.fillStyle = F.Fish.shade(iu.def.color, -0.2);
            ctx.beginPath();
            ctx.moveTo(-sz, 0);
            ctx.lineTo(-sz * 1.6, -sz * 0.5);
            ctx.lineTo(-sz * 1.6, sz * 0.5);
            ctx.closePath();
            ctx.fill();
            // teeth
            ctx.fillStyle = '#ffffff';
            ctx.beginPath();
            ctx.moveTo(sz, -sz * 0.1);
            ctx.lineTo(sz * 0.7, -sz * 0.3);
            ctx.lineTo(sz * 0.8, -sz * 0.1);
            ctx.closePath();
            ctx.fill();
        }
        // eye
        ctx.fillStyle = '#ffffff';
        ctx.beginPath();
        ctx.arc(sz * 0.45, -sz * 0.2, sz * 0.14, 0, Math.PI * 2);
        ctx.fill();
        ctx.fillStyle = '#200008';
        ctx.beginPath();
        ctx.arc(sz * 0.48, -sz * 0.2, sz * 0.08, 0, Math.PI * 2);
        ctx.fill();
        ctx.restore();

        // HP bar
        if (!iu.dead && iu.hpMax > 1) {
            var w = 32;
            var pct = iu.hp / iu.hpMax;
            ctx.fillStyle = 'rgba(20,5,5,0.8)';
            ctx.fillRect(iu.x - w/2, iu.y - iu.def.size - 10, w, 4);
            ctx.fillStyle = pct > 0.5 ? '#50e070' : (pct > 0.25 ? '#f0c050' : '#e05050');
            ctx.fillRect(iu.x - w/2, iu.y - iu.def.size - 10, w * pct, 4);
        }
    }

    // Choose a wave set for a given day.
    function spawnWaveForDay(day, rng) {
        rng = rng || Math.random;
        var list = [];
        if (day % 5 === 0) {
            // Boss day
            list.push({ t: 4000, type: 'wrecker' });
            for (var i = 0; i < 3; i++) list.push({ t: 10000 + i * 3000, type: 'swarmer' });
            return list;
        }
        var count = 1 + Math.min(5, Math.floor(day / 2));
        var types = ['snatcher'];
        if (day >= 2) types.push('siphoner');
        if (day >= 3) types.push('swarmer');
        if (day >= 4) types.push('drifter');
        for (var k = 0; k < count; k++) {
            var type = types[Math.floor(rng() * types.length)];
            list.push({ t: 20000 + k * 18000 + Math.floor(rng() * 4000), type: type });
        }
        return list;
    }

    return {
        TYPES: TYPES,
        makeIntruder: makeIntruder,
        step: step,
        draw: draw,
        damage: damage,
        spawnWaveForDay: spawnWaveForDay
    };
})();
