// fish.js — fish entities: movement, hunger, feeding, coin dropping, eggs.
'use strict';
var F = window.F = window.F || {};

F.Fish = (function () {
    // Internal state lives in Game; this module just exports pure(ish) helpers.
    function makeFish(tierId, x, y) {
        var t = F.Economy.fishById(tierId) || F.Economy.FISH_TIERS[0];
        return {
            tier: t.id,
            def: t,
            x: x, y: y,
            vx: (Math.random() < 0.5 ? -1 : 1) * t.speed,
            vy: (Math.random() - 0.5) * 10,
            age: 0,
            size: t.size * 0.55,       // starts small (baby), grows to size
            grown: false,
            hunger: 100,              // 0 = starving, 100 = full
            justAteT: 0,              // seconds since last ate; lays coin when > feedMs/1000 ???
            eatCooldown: 0,           // short delay after eating to avoid instant re-chase
            coinTimer: 0,             // time until next coin drop (seconds) after being fed
            hasFood: false,           // true while the fish has eaten and is pending coin drop
            target: null,             // pellet ref
            dead: false,
            floatUp: 0,               // when dead, time-to-despawn
            wag: Math.random() * Math.PI * 2,
            eggTimer: 20 + Math.random() * 20  // seconds until next egg chance
        };
    }

    // Step a single fish for `dt` ms. Delegate world state via ctx.
    // ctx provides: Wd, Hd, tankTop, tankBottom, pellets[], fish[],
    //               onCoinDrop(x,y,tier,value), onFishDie(i), onEggLay(x,y),
    //               filterMult()
    function step(f, ms, ctx) {
        if (f.dead) {
            f.floatUp += ms;
            f.y -= 0.04 * ms;
            if (f.y < ctx.tankTop - 20) f._despawn = true;
            return;
        }
        f.age += ms;
        // Hunger decay (modified by filter upgrade)
        var decayPerSec = 1.6 * ctx.filterMult();
        if (F.Economy.settings.difficulty === 0) decayPerSec *= 0.7;
        else if (F.Economy.settings.difficulty === 2) decayPerSec *= 1.3;
        f.hunger = Math.max(0, f.hunger - decayPerSec * (ms/1000));
        if (f.hunger <= 0) {
            // Starve over time: if starved for 12s continuously, die.
            f.starveAccum = (f.starveAccum || 0) + ms;
            if (f.starveAccum > 12000) {
                f.dead = true;
                return;
            }
        } else {
            f.starveAccum = 0;
        }

        // Growth
        if (!f.grown) {
            f.size = Math.min(f.def.size, f.size + 0.0008 * ms);
            if (f.size >= f.def.size - 0.2) f.grown = true;
        }

        // Eating cooldown timer
        if (f.eatCooldown > 0) f.eatCooldown = Math.max(0, f.eatCooldown - ms);

        // Coin drop cycle
        if (f.hasFood) {
            f.coinTimer -= ms;
            if (f.coinTimer <= 0) {
                var tier = f.def.coinTier;
                var value = F.Economy.coinValue(tier);
                // Diamond drop chance for pearlscale
                if (f.def.diamondDrop && Math.random() < 0.3) {
                    value = value * 2;
                    tier = 6;
                }
                // Pellet boost from current pellet tier
                var pellet = F.Economy.pelletById(ctx.pelletTier()) || F.Economy.PELLET_TIERS[0];
                value = Math.round(value * pellet.coinBoost);
                ctx.onCoinDrop(f.x, f.y + 10, tier, value);
                f.hasFood = false;
            }
        }

        // Egg laying for tier-4
        if (f.def.eggLayer && f.grown && f.hunger > 40) {
            f.eggTimer -= ms/1000;
            if (f.eggTimer <= 0) {
                ctx.onEggLay(f.x, f.y);
                f.eggTimer = 30 + Math.random() * 20;
            }
        }

        // Seek pellet if hungry and not on cooldown
        if (!f.target && f.hunger < 85 && f.eatCooldown <= 0) {
            f.target = findNearestPellet(f, ctx.pellets);
        }
        if (f.target && (!f.target.alive || f.target._consumed)) f.target = null;

        // Move
        if (f.target) {
            var dx = f.target.x - f.x, dy = f.target.y - f.y;
            var d = Math.hypot(dx, dy) || 1;
            var spd = f.def.speed * 1.6;
            f.vx = (dx / d) * spd;
            f.vy = (dy / d) * spd;
            if (d < 14) {
                // Eat!
                f.target._consumed = true;
                var pellet2 = F.Economy.pelletById(ctx.pelletTier()) || F.Economy.PELLET_TIERS[0];
                f.hunger = Math.min(100, f.hunger + pellet2.restore);
                f.hasFood = true;
                f.coinTimer = f.def.feedMs;
                f.eatCooldown = 600;
                f.target = null;
                if (ctx.onFishAte) ctx.onFishAte(f);
            }
        } else {
            // Idle wander
            f.vy += (Math.random() - 0.5) * 10;
            f.vy *= 0.98;
            // Occasional direction flip
            if (Math.random() < 0.001 * ms) f.vx = -f.vx;
        }
        f.x += f.vx * (ms/1000);
        f.y += f.vy * (ms/1000);

        // Walls
        if (f.x < ctx.tankLeft + 20) { f.x = ctx.tankLeft + 20; f.vx = Math.abs(f.vx); }
        if (f.x > ctx.tankRight - 20) { f.x = ctx.tankRight - 20; f.vx = -Math.abs(f.vx); }
        if (f.y < ctx.tankTop + 20) { f.y = ctx.tankTop + 20; f.vy = Math.abs(f.vy); }
        if (f.y > ctx.tankBottom - 20) { f.y = ctx.tankBottom - 20; f.vy = -Math.abs(f.vy); }

        f.wag += ms/120;
    }

    function findNearestPellet(f, pellets) {
        var best = null, bd = 99999;
        for (var i = 0; i < pellets.length; i++) {
            var p = pellets[i];
            if (!p.alive || p._consumed) continue;
            var d = Math.hypot(p.x - f.x, p.y - f.y);
            if (d < bd) { bd = d; best = p; }
        }
        return best;
    }

    function draw(ctx, f) {
        ctx.save();
        ctx.translate(f.x, f.y);
        var facing = f.vx < 0 ? -1 : 1;
        ctx.scale(facing, 1);
        var sz = f.size;
        var wag = Math.sin(f.wag) * 0.3;

        // hunger desaturation
        var hungry = f.hunger < 30;
        var alpha = f.dead ? 0.5 : 1.0;
        ctx.globalAlpha = alpha;

        // tail
        ctx.fillStyle = shade(f.def.color, -0.2);
        ctx.beginPath();
        ctx.moveTo(-sz, 0);
        ctx.lineTo(-sz * 1.8, -sz * 0.7 + wag * sz);
        ctx.lineTo(-sz * 1.8, sz * 0.7 + wag * sz);
        ctx.closePath();
        ctx.fill();

        // body
        ctx.fillStyle = hungry ? shade(f.def.color, -0.35) : f.def.color;
        ctx.beginPath();
        ctx.ellipse(0, 0, sz, sz * 0.6, 0, 0, Math.PI * 2);
        ctx.fill();

        // belly stripe
        ctx.fillStyle = shade(f.def.color, 0.3);
        ctx.beginPath();
        ctx.ellipse(0, sz * 0.25, sz * 0.7, sz * 0.2, 0, 0, Math.PI * 2);
        ctx.fill();

        // top fin
        ctx.fillStyle = shade(f.def.color, -0.2);
        ctx.beginPath();
        ctx.moveTo(-sz * 0.3, -sz * 0.55);
        ctx.lineTo(sz * 0.2, -sz * 0.55);
        ctx.lineTo(0, -sz * 1.0);
        ctx.closePath();
        ctx.fill();

        // eye
        ctx.fillStyle = '#ffffff';
        ctx.beginPath();
        ctx.arc(sz * 0.4, -sz * 0.1, sz * 0.16, 0, Math.PI * 2);
        ctx.fill();
        ctx.fillStyle = '#0a1020';
        ctx.beginPath();
        ctx.arc(sz * 0.46, -sz * 0.1, sz * 0.08, 0, Math.PI * 2);
        ctx.fill();

        ctx.restore();

        // hunger indicator (exclamation bubble)
        if (!f.dead && hungry) {
            ctx.save();
            ctx.globalAlpha = 0.7 + 0.3 * Math.sin(f.wag * 2);
            W.Text.drawCentered(ctx, '!', f.x, f.y - sz - 14, 2, '#ff9a6a');
            ctx.restore();
        }
    }

    // simple hex color shading
    function shade(hex, pct) {
        var c = hex.replace('#','');
        var r = parseInt(c.substr(0,2),16);
        var g = parseInt(c.substr(2,2),16);
        var b = parseInt(c.substr(4,2),16);
        function m(x) { return Math.max(0, Math.min(255, Math.round(x + (pct > 0 ? (255-x) : x) * pct))); }
        return 'rgb(' + m(r) + ',' + m(g) + ',' + m(b) + ')';
    }

    return {
        makeFish: makeFish,
        step: step,
        draw: draw,
        shade: shade
    };
})();
