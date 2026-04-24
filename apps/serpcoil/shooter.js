// shooter.js — fixed center shooter with aim + projectiles.
var SC = SC || {};

SC.Shooter = (function () {
    "use strict";

    var MUZZLE_OFFSET = 28;
    var PROJECTILE_SPEED = 780; // px/s

    // Power-up types
    var PU_NONE = 0;
    var PU_BACKTRACK = 1;
    var PU_BLASTER = 2;
    var PU_COLORSHIFT = 3;
    var PU_SLOWMO = 4;
    var PU_NAMES = ["", "BACKTRACK", "BLASTER", "COLORSHIFT", "SLOW-MO"];

    function create(opts) {
        opts = opts || {};
        var x = opts.x || 0;
        var y = opts.y || 0;
        var palette = opts.palette || [1, 2, 3];
        var aim = 0; // radians
        var rng = opts.rng || Math.random;
        var cooldown = 0;
        var fireDelay = 220; // ms
        var current = randomColor();
        var currentPU = PU_NONE;
        var next = randomColor();
        var nextPU = PU_NONE;
        var popsSincePU = 0;
        var popsForNextPU = 28 + ((rng() * 8) | 0);

        var projectiles = []; // {x, y, vx, vy, color, pu, life}

        function randomColor() {
            return palette[(rng() * palette.length) | 0];
        }

        function setPosition(px, py) { x = px; y = py; }
        function setPalette(p) { palette = p.slice(); }
        function setAim(angle) { aim = angle; }
        function aimAt(px, py) { aim = Math.atan2(py - y, px - x); }

        function maybeInjectPU() {
            popsSincePU++;
            if (popsSincePU >= popsForNextPU) {
                popsSincePU = 0;
                popsForNextPU = 28 + ((rng() * 12) | 0);
                // Apply to "next" slot so the player gets notice.
                nextPU = 1 + ((rng() * 4) | 0); // 1..4
                return true;
            }
            return false;
        }

        function tick(dtMs) {
            if (cooldown > 0) cooldown -= dtMs;
            var dtS = dtMs / 1000;
            for (var i = projectiles.length - 1; i >= 0; i--) {
                var p = projectiles[i];
                p.x += p.vx * dtS;
                p.y += p.vy * dtS;
                p.life -= dtMs;
                if (p.life <= 0) projectiles.splice(i, 1);
            }
        }

        function fire() {
            if (cooldown > 0) return null;
            cooldown = fireDelay;
            var mx = x + Math.cos(aim) * MUZZLE_OFFSET;
            var my = y + Math.sin(aim) * MUZZLE_OFFSET;
            var proj = {
                x: mx, y: my,
                vx: Math.cos(aim) * PROJECTILE_SPEED,
                vy: Math.sin(aim) * PROJECTILE_SPEED,
                color: current,
                pu: currentPU,
                life: 2400
            };
            projectiles.push(proj);
            current = next;
            currentPU = nextPU;
            next = randomColor();
            nextPU = PU_NONE;
            return proj;
        }

        function swap() {
            var tc = current, tp = currentPU;
            current = next; currentPU = nextPU;
            next = tc; nextPU = tp;
        }

        // Refresh current/next color — used when level palette changes mid-game.
        function refreshColors() {
            if (palette.indexOf(current) < 0) current = randomColor();
            if (palette.indexOf(next) < 0) next = randomColor();
        }

        // Remove projectile by reference.
        function removeProjectile(proj) {
            var idx = projectiles.indexOf(proj);
            if (idx >= 0) projectiles.splice(idx, 1);
        }

        function clearProjectiles() { projectiles.length = 0; }

        // --- rendering ---
        function draw(ctx) {
            // Base plinth
            ctx.save();
            ctx.translate(x, y);
            // outer glow
            var grad = ctx.createRadialGradient(0, 0, 6, 0, 0, 50);
            grad.addColorStop(0, "rgba(154,86,255,0.4)");
            grad.addColorStop(1, "rgba(154,86,255,0)");
            ctx.fillStyle = grad;
            ctx.fillRect(-50, -50, 100, 100);

            // rotating base
            ctx.rotate(aim);
            // Body
            ctx.fillStyle = "#2a1a4a";
            ctx.strokeStyle = "#9a56ff";
            ctx.lineWidth = 2;
            ctx.beginPath();
            ctx.moveTo(-22, -18);
            ctx.lineTo(18, -10);
            ctx.lineTo(28, 0);
            ctx.lineTo(18, 10);
            ctx.lineTo(-22, 18);
            ctx.closePath();
            ctx.fill();
            ctx.stroke();
            // Muzzle
            ctx.fillStyle = "#1a0e2e";
            ctx.fillRect(22, -4, 10, 8);
            ctx.restore();

            // Base circle + current orb
            ctx.save();
            ctx.fillStyle = "#1a0e2e";
            ctx.strokeStyle = "#9a56ff";
            ctx.lineWidth = 2;
            ctx.beginPath();
            ctx.arc(x, y, 16, 0, Math.PI * 2);
            ctx.fill(); ctx.stroke();
            SC.Chain.COLORS[current] && drawShooterOrb(ctx, x, y, current, currentPU);
            ctx.restore();

            // Next preview behind shooter
            var back = { x: x - Math.cos(aim) * 38, y: y - Math.sin(aim) * 38 };
            ctx.save();
            ctx.globalAlpha = 0.8;
            ctx.fillStyle = "#0a0618";
            ctx.beginPath();
            ctx.arc(back.x, back.y, 13, 0, Math.PI * 2);
            ctx.fill();
            if (SC.Chain.COLORS[next]) drawShooterOrb(ctx, back.x, back.y, next, nextPU, 11);
            ctx.restore();

            // Projectiles
            for (var i = 0; i < projectiles.length; i++) {
                var p = projectiles[i];
                drawShooterOrb(ctx, p.x, p.y, p.color, p.pu, 14);
            }
        }

        function drawShooterOrb(ctx, ox, oy, color, pu, radius) {
            var r = radius || 12;
            var c = SC.Chain.COLORS[color];
            if (!c) return;
            ctx.fillStyle = c.hex;
            ctx.beginPath();
            ctx.arc(ox, oy, r, 0, Math.PI * 2);
            ctx.fill();
            ctx.strokeStyle = "rgba(255,255,255,0.4)";
            ctx.lineWidth = 1;
            ctx.beginPath();
            ctx.arc(ox, oy, r - 0.5, 0, Math.PI * 2);
            ctx.stroke();
            // Powerup glyph overlay
            if (pu && pu !== PU_NONE) {
                ctx.fillStyle = "#fff";
                ctx.font = "bold " + Math.floor(r * 0.95) + "px Consolas, monospace";
                ctx.textAlign = "center";
                ctx.textBaseline = "middle";
                var glyph = pu === PU_BACKTRACK ? "←" :
                            pu === PU_BLASTER   ? "*"     :
                            pu === PU_COLORSHIFT? "~"     :
                            pu === PU_SLOWMO    ? "◷" : "";
                ctx.fillText(glyph, ox, oy + 1);
            }
        }

        return {
            setPosition: setPosition,
            setAim: setAim,
            aimAt: aimAt,
            fire: fire,
            swap: swap,
            tick: tick,
            draw: draw,
            projectiles: function () { return projectiles; },
            removeProjectile: removeProjectile,
            clearProjectiles: clearProjectiles,
            current: function () { return current; },
            next: function () { return next; },
            currentPU: function () { return currentPU; },
            nextPU: function () { return nextPU; },
            setCurrent: function (c, pu) { current = c; currentPU = pu || PU_NONE; },
            setNext: function (c, pu) { next = c; nextPU = pu || PU_NONE; },
            setPalette: setPalette,
            refreshColors: refreshColors,
            maybeInjectPU: maybeInjectPU,
            aim: function () { return aim; },
            x: function () { return x; },
            y: function () { return y; },
            PU_NONE: PU_NONE,
            PU_BACKTRACK: PU_BACKTRACK,
            PU_BLASTER: PU_BLASTER,
            PU_COLORSHIFT: PU_COLORSHIFT,
            PU_SLOWMO: PU_SLOWMO,
            PU_NAMES: PU_NAMES
        };
    }

    return {
        create: create,
        PU_NONE: PU_NONE,
        PU_BACKTRACK: PU_BACKTRACK,
        PU_BLASTER: PU_BLASTER,
        PU_COLORSHIFT: PU_COLORSHIFT,
        PU_SLOWMO: PU_SLOWMO,
        PU_NAMES: PU_NAMES
    };
})();
