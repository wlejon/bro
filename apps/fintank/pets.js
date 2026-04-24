// pets.js — persistent helpers that automate chores.
'use strict';
var F = window.F = window.F || {};

F.Pets = (function () {
    // ctx provides: Wd, Hd, tankLeft/Right/Top/Bottom, fish, pellets, coins, intruders,
    //               addPellet(x), addCoinAt(x,y,tier,value), collectCoin(c), damageIntruder(i,amt)
    function makeActivePet(petId) {
        var def = F.Economy.petById(petId);
        if (!def) return null;
        return {
            id: petId,
            def: def,
            x: 600, y: 500,
            tx: 600, ty: 500,
            vx: 0, vy: 0,
            actCooldown: randCooldown(petId),
            wag: 0,
            phase: 'idle'
        };
    }

    function randCooldown(id) {
        if (id === 'bubbler')     return 5000  + Math.random() * 3000;
        if (id === 'coinkeeper')  return 4000  + Math.random() * 2000;
        if (id === 'pufferguard') return 1400  + Math.random() * 1200;
        if (id === 'alchem')      return 10000 + Math.random() * 6000;
        if (id === 'sprout')      return 9000  + Math.random() * 6000;
        return 5000;
    }

    function step(p, ms, ctx) {
        if (!p) return;
        p.wag += ms / 120;
        p.actCooldown -= ms;

        // Movement towards target
        var dx = p.tx - p.x, dy = p.ty - p.y;
        var d = Math.hypot(dx, dy);
        if (d > 2) {
            var spd = 80;
            p.vx = dx / d * spd;
            p.vy = dy / d * spd;
            p.x += p.vx * (ms/1000);
            p.y += p.vy * (ms/1000);
        } else {
            // idle wobble
            p.x += Math.sin(p.wag) * 0.2;
        }

        if (p.actCooldown > 0) return;

        // Act
        if (p.id === 'bubbler') {
            // find a hungry fish and drop pellet there
            var hungry = null, hmin = 101;
            for (var i = 0; i < ctx.fish.length; i++) {
                var f = ctx.fish[i];
                if (f.dead) continue;
                if (f.hunger < hmin && f.hunger < 60) { hmin = f.hunger; hungry = f; }
            }
            if (hungry) {
                ctx.addPellet(hungry.x, ctx.tankTop + 20);
                p.tx = hungry.x; p.ty = hungry.y - 30;
            }
            p.actCooldown = randCooldown(p.id);
        } else if (p.id === 'coinkeeper') {
            // Collect oldest coin
            var oldest = null, omax = 0;
            for (var j = 0; j < ctx.coins.length; j++) {
                var c = ctx.coins[j];
                if (!c.alive || c._consumed) continue;
                if (c.age > omax) { omax = c.age; oldest = c; }
            }
            if (oldest && oldest.age > 3) {
                p.tx = oldest.x; p.ty = oldest.y;
                if (Math.hypot(p.x - oldest.x, p.y - oldest.y) < 20) {
                    ctx.collectCoin(oldest);
                    p.actCooldown = randCooldown(p.id);
                } else {
                    p.actCooldown = 100; // retry soon
                }
            } else {
                p.actCooldown = randCooldown(p.id);
            }
        } else if (p.id === 'pufferguard') {
            // Attack nearest intruder
            var nearest = null, ndist = 99999;
            for (var k = 0; k < ctx.intruders.length; k++) {
                var iu = ctx.intruders[k];
                if (iu.dead) continue;
                var di = Math.hypot(iu.x - p.x, iu.y - p.y);
                if (di < ndist) { ndist = di; nearest = iu; }
            }
            if (nearest) {
                p.tx = nearest.x; p.ty = nearest.y;
                if (ndist < 40) {
                    ctx.damageIntruder(nearest, 1);
                    p.actCooldown = randCooldown(p.id);
                } else {
                    p.actCooldown = 200;
                }
            } else {
                // wander
                p.tx = ctx.tankLeft + Math.random() * (ctx.tankRight - ctx.tankLeft);
                p.ty = ctx.tankTop + Math.random() * (ctx.tankBottom - ctx.tankTop);
                p.actCooldown = randCooldown(p.id);
            }
        } else if (p.id === 'alchem') {
            // Upgrade a random small coin to the next tier
            var cands = [];
            for (var m = 0; m < ctx.coins.length; m++) {
                var cc = ctx.coins[m];
                if (cc.alive && !cc._consumed && cc.tier < 5) cands.push(cc);
            }
            if (cands.length) {
                var pick = cands[Math.floor(Math.random() * cands.length)];
                p.tx = pick.x; p.ty = pick.y;
                pick.tier = Math.min(6, pick.tier + 1);
                pick.value = F.Economy.coinValue(pick.tier);
                if (ctx.onAlchemUpgrade) ctx.onAlchemUpgrade(pick);
            }
            p.actCooldown = randCooldown(p.id);
        } else if (p.id === 'sprout') {
            // Drop a free pellet
            var px = ctx.tankLeft + 30 + Math.random() * (ctx.tankRight - ctx.tankLeft - 60);
            ctx.addPellet(px, ctx.tankTop + 20);
            p.tx = px; p.ty = ctx.tankTop + 40;
            p.actCooldown = randCooldown(p.id);
        }
    }

    function draw(ctx, p) {
        if (!p) return;
        ctx.save();
        ctx.translate(p.x, p.y);
        var wobble = Math.sin(p.wag) * 2;
        ctx.translate(0, wobble);

        if (p.id === 'bubbler') {
            // round pale blue blob with bubble trail
            ctx.fillStyle = '#9fe0ff';
            ctx.beginPath(); ctx.arc(0, 0, 14, 0, Math.PI * 2); ctx.fill();
            ctx.fillStyle = '#ffffff';
            ctx.beginPath(); ctx.arc(-4, -4, 3, 0, Math.PI * 2); ctx.fill();
            drawEyes(ctx, 5);
        } else if (p.id === 'coinkeeper') {
            // pouch-looking round yellow
            ctx.fillStyle = '#f2c95b';
            ctx.beginPath(); ctx.arc(0, 0, 14, 0, Math.PI * 2); ctx.fill();
            ctx.fillStyle = '#a6751a';
            ctx.fillRect(-8, 4, 16, 4);
            drawEyes(ctx, 4);
        } else if (p.id === 'pufferguard') {
            // spiky green puffer
            ctx.fillStyle = '#6ad14a';
            ctx.beginPath(); ctx.arc(0, 0, 16, 0, Math.PI * 2); ctx.fill();
            ctx.fillStyle = '#4a9a36';
            for (var s = 0; s < 10; s++) {
                var a = s / 10 * Math.PI * 2;
                ctx.beginPath();
                ctx.moveTo(Math.cos(a) * 14, Math.sin(a) * 14);
                ctx.lineTo(Math.cos(a) * 22, Math.sin(a) * 22);
                ctx.lineTo(Math.cos(a + 0.2) * 14, Math.sin(a + 0.2) * 14);
                ctx.closePath();
                ctx.fill();
            }
            drawEyes(ctx, 5);
        } else if (p.id === 'alchem') {
            // purple wizardly
            ctx.fillStyle = '#b06acb';
            ctx.beginPath(); ctx.arc(0, 0, 14, 0, Math.PI * 2); ctx.fill();
            ctx.fillStyle = '#702a90';
            ctx.beginPath();
            ctx.moveTo(-10, -8);
            ctx.lineTo(0, -20);
            ctx.lineTo(10, -8);
            ctx.closePath(); ctx.fill();
            drawEyes(ctx, 4);
        } else if (p.id === 'sprout') {
            // green with leaf
            ctx.fillStyle = '#a8f06c';
            ctx.beginPath(); ctx.arc(0, 0, 13, 0, Math.PI * 2); ctx.fill();
            ctx.fillStyle = '#60a830';
            ctx.beginPath();
            ctx.ellipse(0, -16, 5, 10, 0, 0, Math.PI * 2);
            ctx.fill();
            drawEyes(ctx, 4);
        } else {
            ctx.fillStyle = '#cccccc';
            ctx.beginPath(); ctx.arc(0, 0, 14, 0, Math.PI * 2); ctx.fill();
        }
        ctx.restore();
    }

    function drawEyes(ctx, r) {
        ctx.fillStyle = '#ffffff';
        ctx.beginPath(); ctx.arc(-r, -2, 3, 0, Math.PI * 2); ctx.fill();
        ctx.beginPath(); ctx.arc(r, -2, 3, 0, Math.PI * 2); ctx.fill();
        ctx.fillStyle = '#000000';
        ctx.beginPath(); ctx.arc(-r + 1, -2, 1.5, 0, Math.PI * 2); ctx.fill();
        ctx.beginPath(); ctx.arc(r + 1, -2, 1.5, 0, Math.PI * 2); ctx.fill();
    }

    return {
        makeActivePet: makeActivePet,
        step: step,
        draw: draw
    };
})();
