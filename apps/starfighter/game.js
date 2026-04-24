// game.js — Core game state, update, and draw orchestration.
//
// Rail-shooter mechanics:
//   • Player sits at the camera origin (0,0,0), facing +Z.
//   • Yoke deflects the ship within a bounded envelope (shipX/shipY).
//   • The reticle leads the ship by a small lag; aim is done with the reticle.
//   • Lasers are hitscan along a ray from the ship through the reticle;
//     four cosmetic wingtip bolts travel toward the convergence point.
//   • Enemies advance along parametric flight paths and spawn bolts that
//     travel toward the player's position at fire time.
var N = N || {};

N.Game = (function() {
    "use strict";

    // --- Tunables ----------------------------------------------------------
    var MAX_SHIELDS      = 6;
    var START_SHIELDS    = 6;
    // At vfov=60°, the half-width of the visible cone at depth z is
    // (W/2)/focal * z = (W/H) * tan(vfov/2) * z. For a 1024x768 canvas
    // that's ≈0.77 * z horizontally. We size the yoke envelope so the
    // reticle stays well inside the visible cone at RETICLE_Z.
    var YOKE_MAX_DEFLECT = 22;     // world units the ship can move off-center
    var RETICLE_LAG_MS   = 140;
    var FORWARD_SPEED    = 0.07;   // space-rail star advance rate

    // Parallax: world view slides opposite the ship position by this factor
    // of shipX/shipY. Arcade yoke-look feel — subtle, not a free-look camera.
    var PARALLAX         = 0.35;
    var RETICLE_Z        = 40;     // depth at which laser beams converge
    var FIRE_COOLDOWN_MS = 170;
    var BOLT_VISUAL_LIFE = 180;    // ms: cosmetic player bolts
    var ENEMY_BOLT_SPEED = 0.18;   // world units / ms (approach rate toward ship)
    var PLAYER_HIT_RADIUS = 3.0;   // enemy-bolt vs player
    var MAX_EXPLOSIONS   = 24;

    // --- State -------------------------------------------------------------
    var state = null;

    function resetState() {
        state = {
            score: 0, loop: 1, sector: 1,
            wave: N.Waves.startingWave(),
            shields: START_SHIELDS,
            gameOver: false, paused: false,
            victoryPending: false,

            shipX: 0, shipY: 0,
            reticleX: 0, reticleY: 0,
            yokeX: 0, yokeY: 0,
            firePressed: false,
            fireCooldown: 0,
            targetingComputer: true,

            W: 1024, H: 768,

            enemies: [],
            playerBolts: [],        // visual 4-beam lasers
            enemyBolts: [],         // tracking bolts toward player
            explosions: [],

            radioText: "", radioUntil: 0,
            lockActive: false,
            sectorT: 0,

            wavescript: null        // current wave state object (from waves.js)
        };
    }

    // --- Public setters ----------------------------------------------------
    function setViewport(W, H) {
        if (!state) return;
        state.W = W; state.H = H;
        N.Render.setViewport(W, H);
    }
    function setYoke(nx, ny) {
        if (!state) return;
        state.yokeX = Math.max(-1, Math.min(1, nx));
        state.yokeY = Math.max(-1, Math.min(1, ny));
    }
    function setFire(p) { if (state) state.firePressed = !!p; }
    function toggleTargetingComputer() {
        if (!state) return;
        state.targetingComputer = !state.targetingComputer;
        radio(state.targetingComputer ? "TARGETING COMPUTER: ON" : "TARGETING COMPUTER: OFF", 1400);
    }
    function radio(text, ms) {
        if (!state) return;
        state.radioText = text;
        state.radioUntil = performance.now() + (ms || 2000);
    }

    // --- Campaign flow -----------------------------------------------------
    function start(W, H) {
        resetState();
        setViewport(W, H);
        N.Render.initStars();
        enterWave();
        N.Audio.sfxWave();
    }

    function enterWave() {
        state.sectorT = 0;
        state.lockActive = false;
        state.enemies.length = 0;
        state.playerBolts.length = 0;
        state.enemyBolts.length = 0;
        state.explosions.length = 0;
        state.wavescript = N.Waves.create(state.wave, api);
        if (state.wave === N.Waves.SPACE) {
            radio("SECTOR " + state.loop + "-1  ::  ENEMY FIGHTERS INBOUND", 2600);
        } else if (state.wave === N.Waves.SURFACE) {
            radio("SECTOR " + state.loop + "-2  ::  CITADEL SURFACE  ::  TOWERS HOT", 2600);
        } else if (state.wave === N.Waves.TRENCH) {
            radio("SECTOR " + state.loop + "-3  ::  TRENCH APPROACH  ::  HIT THE VENT", 2600);
        }
    }

    // Per-sector completion bonus. Base points scale with loop number so
    // score per hour rises with difficulty, not just per-kill values.
    var SECTOR_BONUS = { space: 5000, surface: 10000, trench: 15000 };
    var SHIELD_BONUS_PER = 2500;

    function completeWave() {
        // Bonus shield (if we're below cap).
        if (state.shields < MAX_SHIELDS) {
            state.shields = Math.min(MAX_SHIELDS, state.shields + N.Waves.waveCompleteBonusShields());
            N.Audio.sfxBonusShield();
        }

        // Sector bonus scaled by loop.
        var bonus = (SECTOR_BONUS[state.wave] || 0) * N.Waves.loopScale(state.loop);
        bonus = Math.round(bonus);
        if (bonus > 0) {
            addScore(bonus);
            radio("SECTOR CLEAR  +" + bonus, 2200);
        }

        var next = N.Waves.nextWave(state.wave);
        if (!next) {
            // Trench victory: additional shield bonus — reward surviving the climax.
            var sb = state.shields * SHIELD_BONUS_PER * N.Waves.loopScale(state.loop);
            sb = Math.round(sb);
            if (sb > 0) addScore(sb);
            state.victoryPending = true;
            return;
        }
        state.wave = next;
        state.sector = (state.wave === N.Waves.SPACE ? 1 : state.wave === N.Waves.SURFACE ? 2 : 3);
        enterWave();
        N.Audio.sfxWave();
    }

    function advanceLoop() {
        state.loop += 1;
        state.sector = 1;
        state.wave = N.Waves.SPACE;
        state.victoryPending = false;
        enterWave();
        N.Audio.sfxWave();
    }

    function takeDamage(amount) {
        amount = amount || 1;
        state.shields -= amount;
        N.Render.shake(8, 260);
        N.Render.setJitter(1.5);
        N.Render.flash("#f33", 220);
        N.Audio.sfxShieldHit();
        setTimeout(function() { N.Render.setJitter(0); }, 220);
        if (state.shields <= 0) {
            state.shields = 0;
            state.gameOver = true;
            N.Audio.sfxShipExplode();
            N.Render.shake(16, 900);
        }
    }

    function addScore(pts) { state.score += pts | 0; }

    // --- Fire handling -----------------------------------------------------
    // The ship's four wingtips in world space (offset by ship position).
    // Beams converge at (reticleX, reticleY, RETICLE_Z).
    var WINGTIP_OFFSETS = [
        { x:  6, y:  1.5 }, { x: -6, y:  1.5 },
        { x:  6, y: -1.5 }, { x: -6, y: -1.5 }
    ];

    function fireLasers() {
        if (state.fireCooldown > 0) return;
        state.fireCooldown = FIRE_COOLDOWN_MS;

        // Spawn 4 cosmetic bolts (origin → convergence).
        var tx = state.reticleX, ty = state.reticleY;
        for (var i = 0; i < WINGTIP_OFFSETS.length; i++) {
            var w = WINGTIP_OFFSETS[i];
            state.playerBolts.push({
                ox: state.shipX + w.x,
                oy: state.shipY + w.y,
                oz: 1.0,               // near the cockpit
                tx: tx, ty: ty, tz: RETICLE_Z,
                t: 0, life: BOLT_VISUAL_LIFE,
                color: "#f44"
            });
        }
        N.Audio.sfxLaser();

        // Hitscan: single ray from ship through reticle, out to max depth.
        // Find the closest enemy whose bounding sphere is intersected by the ray.
        // Ray origin: (shipX, shipY, 0). Dir toward (reticleX, reticleY, RETICLE_Z).
        var ox = state.shipX, oy = state.shipY, oz = 0;
        var dx = tx - ox, dy = ty - oy, dz = RETICLE_Z - oz;
        var dlen = Math.sqrt(dx*dx + dy*dy + dz*dz);
        if (dlen < 0.001) return;
        dx /= dlen; dy /= dlen; dz /= dlen;

        var bestT = Infinity;
        var bestEnemy = null;
        for (var j = 0; j < state.enemies.length; j++) {
            var e = state.enemies[j];
            if (e.dead || e.hp <= 0) continue;
            // Fireballs can't be shot.
            if (e.kind === "fireball") continue;
            // Solve ray-sphere: |O + tD - C|^2 = r^2.
            var cx = e.x - ox, cy = e.y - oy, cz = e.z - oz;
            var tca = cx * dx + cy * dy + cz * dz;
            if (tca < 0) continue;
            var d2 = cx*cx + cy*cy + cz*cz - tca * tca;
            var r = e.radius + 0.5;
            if (d2 > r * r) continue;
            var thc = Math.sqrt(r * r - d2);
            var t0 = tca - thc;
            if (t0 < bestT) { bestT = t0; bestEnemy = e; }
        }
        if (bestEnemy) {
            if (bestEnemy.kind === "ace") {
                // Ace can't be killed — only flees.
                if (!bestEnemy.flee) {
                    bestEnemy.flee = true;
                    addScore(2000);
                    N.Audio.sfxEnemyHit();
                    radio("BLACK ACE BREAKING OFF", 1600);
                }
            } else if (bestEnemy.kind === "port") {
                resolvePortHit(bestEnemy, ox, oy, oz, dx, dy, dz);
            } else {
                bestEnemy.hp -= 1;
                if (bestEnemy.hp <= 0) {
                    killEnemy(bestEnemy);
                }
            }
        }
    }

    // --- Exhaust-port hit resolution --------------------------------------
    // Called when the hitscan ray intersects the port sphere. Compute the
    // closest-approach distance from the ray to the port center and score
    // bullseye (within innerRadius) or direct-hit (within outerRadius).
    // Targeting computer OFF doubles the bonus — the Trust-Your-Aim idea.
    function resolvePortHit(port, ox, oy, oz, dx, dy, dz) {
        if (port.resolved) return;
        var cx = port.x - ox, cy = port.y - oy, cz = port.z - oz;
        var tca = cx*dx + cy*dy + cz*dz;
        if (tca < 0) return;
        var d2 = cx*cx + cy*cy + cz*cz - tca*tca;
        var d = Math.sqrt(Math.max(0, d2));

        var trust = !state.targetingComputer;
        var trustMult = trust ? 2 : 1;

        if (d <= port.innerRadius) {
            var pts = 100000 * trustMult;
            addScore(pts);
            radio(trust ? "BULLSEYE  ::  TRUST BONUS x2" : "BULLSEYE", 2600);
            N.Audio.sfxBullseye();
        } else {
            var dpts = 25000 * trustMult;
            addScore(dpts);
            radio(trust ? "DIRECT HIT  ::  TRUST BONUS x2" : "DIRECT HIT", 2600);
            N.Audio.sfxDirectHit();
        }
        port.resolved = true;
        port.dead = true;
        spawnExplosion(port.x, port.y, port.z, 3.5);
        N.Render.shake(18, 900);
        N.Render.flash("#fff", 360);
        if (state.wavescript) state.wavescript.portResolved = true;
    }

    function killEnemy(e) {
        e.dead = true;
        addScore(e.score || 0);
        spawnExplosion(e.x, e.y, e.z, e.scale || 1.4);
        N.Audio.sfxEnemyBoom();
    }

    // --- Explosions (line-burst particles) --------------------------------
    function spawnExplosion(x, y, z, scale) {
        if (state.explosions.length >= MAX_EXPLOSIONS) state.explosions.shift();
        var shards = [];
        var n = 8 + ((Math.random() * 4) | 0);
        for (var i = 0; i < n; i++) {
            var a = Math.random() * Math.PI * 2;
            var p = (Math.random() - 0.5) * Math.PI;
            var speed = 0.01 + Math.random() * 0.025;
            shards.push({
                vx: Math.cos(a) * Math.cos(p) * speed * scale,
                vy: Math.sin(p) * speed * scale,
                vz: Math.sin(a) * Math.cos(p) * speed * scale,
                len: 1.2 + Math.random() * 1.8
            });
        }
        state.explosions.push({
            x: x, y: y, z: z,
            shards: shards, t: 0, life: 550
        });
    }

    function updateExplosions(dt) {
        for (var i = state.explosions.length - 1; i >= 0; i--) {
            var e = state.explosions[i];
            e.t += dt;
            if (e.t >= e.life) { state.explosions.splice(i, 1); continue; }
        }
    }

    function drawExplosions(ctx) {
        for (var i = 0; i < state.explosions.length; i++) {
            var e = state.explosions[i];
            var u = e.t / e.life;
            var alpha = 1 - u;
            for (var j = 0; j < e.shards.length; j++) {
                var s = e.shards[j];
                var ax = e.x + s.vx * e.t;
                var ay = e.y + s.vy * e.t;
                var az = e.z + s.vz * e.t;
                var bx = ax - s.vx * 40;
                var by = ay - s.vy * 40;
                var bz = az - s.vz * 40;
                var c = u < 0.3 ? "#ff8" : (u < 0.7 ? "#f84" : "#844");
                N.Render.line(ctx, ax, ay, az, bx, by, bz, c, alpha);
            }
        }
    }

    // --- Player bolts (4-beam visuals) ------------------------------------
    function updatePlayerBolts(dt) {
        for (var i = state.playerBolts.length - 1; i >= 0; i--) {
            var b = state.playerBolts[i];
            b.t += dt;
            if (b.t >= b.life) state.playerBolts.splice(i, 1);
        }
    }
    function drawPlayerBolts(ctx) {
        for (var i = 0; i < state.playerBolts.length; i++) {
            var b = state.playerBolts[i];
            var u = b.t / b.life;
            // Head travels from origin toward target; tail trails behind.
            var headU = Math.min(1, u * 2.2);
            var tailU = Math.max(0, headU - 0.35);
            var hx = b.ox + (b.tx - b.ox) * headU;
            var hy = b.oy + (b.ty - b.oy) * headU;
            var hz = b.oz + (b.tz - b.oz) * headU;
            var tx = b.ox + (b.tx - b.ox) * tailU;
            var ty = b.oy + (b.ty - b.oy) * tailU;
            var tz = b.oz + (b.tz - b.oz) * tailU;
            N.Render.line(ctx, hx, hy, hz, tx, ty, tz, b.color, 1 - u * 0.4);
        }
    }

    // --- Enemy bolts ------------------------------------------------------
    // At spawn the bolt aims at (shipX, shipY, 0). It travels that direction
    // in a straight line; player can evade by moving the yoke after launch.
    function spawnEnemyBolt(fx, fy, fz) {
        var dx = state.shipX - fx;
        var dy = state.shipY - fy;
        var dz = 0 - fz;
        var len = Math.sqrt(dx*dx + dy*dy + dz*dz);
        if (len < 0.001) return;
        dx /= len; dy /= len; dz /= len;
        state.enemyBolts.push({
            x: fx, y: fy, z: fz,
            vx: dx * ENEMY_BOLT_SPEED,
            vy: dy * ENEMY_BOLT_SPEED,
            vz: dz * ENEMY_BOLT_SPEED,
            life: 4500, t: 0,
            color: "#6cf"
        });
        N.Audio.sfxEnemyLaser();
    }

    function updateEnemyBolts(dt) {
        for (var i = state.enemyBolts.length - 1; i >= 0; i--) {
            var b = state.enemyBolts[i];
            b.x += b.vx * dt;
            b.y += b.vy * dt;
            b.z += b.vz * dt;
            b.t += dt;
            if (b.z < 0 || b.t >= b.life) {
                // Check proximity to ship at crossing.
                if (b.z < N.Render.NEAR_Z + 1) {
                    var dx = b.x - state.shipX, dy = b.y - state.shipY;
                    if (dx*dx + dy*dy < PLAYER_HIT_RADIUS * PLAYER_HIT_RADIUS + 2) {
                        takeDamage(1);
                    }
                }
                state.enemyBolts.splice(i, 1);
            }
        }
    }

    function drawEnemyBolts(ctx) {
        for (var i = 0; i < state.enemyBolts.length; i++) {
            var b = state.enemyBolts[i];
            var trail = 12;
            var tx = b.x - b.vx * trail;
            var ty = b.y - b.vy * trail;
            var tz = b.z - b.vz * trail;
            N.Render.line(ctx, b.x, b.y, b.z, tx, ty, tz, b.color, 1);
        }
    }

    // --- Per-frame update --------------------------------------------------
    function update(dt, W, H) {
        if (!state || state.paused || state.gameOver) return;
        if (state.W !== W || state.H !== H) setViewport(W, H);

        state.sectorT += dt;

        var targetX = state.yokeX * YOKE_MAX_DEFLECT;
        var targetY = state.yokeY * YOKE_MAX_DEFLECT;
        var k = Math.min(1, dt / RETICLE_LAG_MS);
        state.shipX += (targetX - state.shipX) * k;
        state.shipY += (targetY - state.shipY) * k;
        state.reticleX = targetX;
        state.reticleY = targetY;

        // Parallax view shift — camera slides with the ship so the world
        // appears to slide opposite the yoke.
        N.Render.setCamera(state.shipX * PARALLAX, state.shipY * PARALLAX);

        N.Render.advanceStars(FORWARD_SPEED * dt);
        N.Render.updateShake(dt);
        N.Render.updateFlash(dt);

        if (state.fireCooldown > 0) state.fireCooldown -= dt;
        if (state.firePressed) fireLasers();

        // Wave logic.
        N.Waves.update(state.wavescript, dt, api);

        // Enemy update + cull.
        for (var i = 0; i < state.enemies.length; i++) {
            N.Enemies.update(state.enemies[i], dt, api);
        }
        for (var j = state.enemies.length - 1; j >= 0; j--) {
            if (state.enemies[j].dead) state.enemies.splice(j, 1);
        }

        updatePlayerBolts(dt);
        updateEnemyBolts(dt);
        updateExplosions(dt);

        if (!state.victoryPending && N.Waves.isComplete(state.wavescript, api)) {
            completeWave();
        }
    }

    // --- Draw --------------------------------------------------------------
    function drawCockpit(ctx, W, H) {
        ctx.strokeStyle = "#3a4";
        ctx.lineWidth = 2;
        ctx.globalAlpha = 0.85;
        var inset = 18;
        var cornerLen = 90;
        ctx.beginPath();
        ctx.moveTo(inset, inset + cornerLen);
        ctx.lineTo(inset, inset);
        ctx.lineTo(inset + cornerLen, inset);
        ctx.moveTo(W - inset - cornerLen, inset);
        ctx.lineTo(W - inset, inset);
        ctx.lineTo(W - inset, inset + cornerLen);
        ctx.moveTo(inset, H - inset - cornerLen);
        ctx.lineTo(inset, H - inset);
        ctx.lineTo(inset + cornerLen, H - inset);
        ctx.moveTo(W - inset - cornerLen, H - inset);
        ctx.lineTo(W - inset, H - inset);
        ctx.lineTo(W - inset, H - inset - cornerLen);
        ctx.stroke();
        ctx.globalAlpha = 1;
    }

    function drawReticle(ctx) {
        // Reticle and ship marker are cockpit-attached — project without
        // the parallax camera offset so they stay anchored to the yoke.
        var pr = N.Render.projectHud(state.reticleX, state.reticleY, RETICLE_Z);
        var ps = N.Render.projectHud(state.shipX, state.shipY, RETICLE_Z);
        if (pr.visible) {
            ctx.strokeStyle = "#ff4";
            ctx.lineWidth = 2;
            var x = pr.x, y = pr.y;
            ctx.beginPath();
            ctx.moveTo(x - 18, y); ctx.lineTo(x - 6, y);
            ctx.moveTo(x + 6,  y); ctx.lineTo(x + 18, y);
            ctx.moveTo(x, y - 18); ctx.lineTo(x, y - 6);
            ctx.moveTo(x, y + 6);  ctx.lineTo(x, y + 18);
            ctx.arc(x, y, 2, 0, Math.PI * 2);
            ctx.stroke();
        }
        if (ps.visible) {
            ctx.strokeStyle = "#6bf";
            ctx.globalAlpha = 0.7;
            ctx.beginPath();
            ctx.moveTo(ps.x, ps.y + 4);
            ctx.lineTo(ps.x - 5, ps.y + 10);
            ctx.lineTo(ps.x + 5, ps.y + 10);
            ctx.closePath();
            ctx.stroke();
            ctx.globalAlpha = 1;
        }
    }

    function draw(ctx, W, H) {
        if (!state) return;
        ctx.fillStyle = "#000000";
        ctx.fillRect(0, 0, W, H);

        N.Waves.draw(state.wavescript, ctx, api);

        ctx.lineWidth = 1.5;

        // Sort enemies back-to-front for stable overdraw.
        var drawList = state.enemies.slice().sort(function(a, b) { return b.z - a.z; });
        for (var i = 0; i < drawList.length; i++) {
            N.Enemies.draw(drawList[i], ctx);
        }

        drawEnemyBolts(ctx);
        drawPlayerBolts(ctx);
        drawExplosions(ctx);

        drawCockpit(ctx, W, H);
        drawReticle(ctx);
        N.Render.drawFlash(ctx);
    }

    // --- HUD queries -------------------------------------------------------
    function getState() {
        if (!state) return null;
        var shieldBar = "";
        for (var i = 0; i < MAX_SHIELDS; i++) shieldBar += (i < state.shields ? "■ " : "□ ");
        return {
            score: state.score,
            loop: state.loop,
            sector: state.sector,
            wave: state.wave,
            waveLabel: state.loop + "-" + state.sector,
            shields: state.shields,
            shieldBar: shieldBar.trim(),
            radio: (performance.now() < state.radioUntil) ? state.radioText : "",
            lockActive: state.lockActive,
            victoryPending: state.victoryPending
        };
    }

    function isGameOver() { return !!(state && state.gameOver); }
    function setPaused(p) { if (state) state.paused = !!p; }

    // --- API surface passed to waves/enemies ------------------------------
    var api = {
        getLoop:         function() { return state.loop; },
        addEnemy:        function(e) { state.enemies.push(e); },
        spawnEnemyBolt:  spawnEnemyBolt,
        hasLiveEnemies:  function() {
            for (var i = 0; i < state.enemies.length; i++) {
                if (!state.enemies[i].dead) return true;
            }
            return false;
        },
        onFireballImpact: function() { takeDamage(1); },
        takeDamage:      takeDamage,
        addScore:        addScore,
        radio:           radio,
        getShip:         function() { return { x: state.shipX, y: state.shipY }; },
        getWave:         function() { return state.wave; },
        getWaveScript:   function() { return state.wavescript; },
        getRailSpeed:    function() {
            if (state.wavescript && state.wavescript.railSpeed != null) return state.wavescript.railSpeed;
            return FORWARD_SPEED;
        },
        _setLock:        function(on) { state.lockActive = !!on; },
        onPortMiss:      function() {
            if (!state.wavescript || state.wavescript.portResolved) return;
            state.wavescript.portResolved = true;
            radio("VENT MISSED  ::  PULL UP", 2400);
        }
    };

    return {
        start: start,
        setViewport: setViewport,
        setYoke: setYoke,
        setFire: setFire,
        toggleTargetingComputer: toggleTargetingComputer,
        update: update,
        draw: draw,
        getState: getState,
        isGameOver: isGameOver,
        setPaused: setPaused,
        advanceLoop: advanceLoop
    };
})();
