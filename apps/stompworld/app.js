// app.js — Stompworld main loop. Wires lib/{loop,input,screens,camera2d,
// tilemap,platformer} together over Art assets and the World 1-1 layout.

(function () {
    'use strict';

    const VIEW_W = 800;
    const VIEW_H = 576;
    const TILE   = 32;

    const canvas = document.getElementById('game');
    canvas.width  = VIEW_W;
    canvas.height = VIEW_H;
    const ctx    = canvas.getContext('2d');
    ctx.imageSmoothingEnabled = false;


    // ── Audio ────────────────────────────────────────────────────────────────
    // All effects go through SFX (apps/lib/audio.js), which wraps the broaudio
    // engine via createVoice + ADSR. Tones are short blips; sequences are
    // arpeggios fired with setTimeout so the C++ engine handles the actual
    // mixing. Keep durations short — the engine compressor swallows overlap.
    SFX.init();
    const Audio = {
        // Player
        jump:    () => SFX.sequence([[520,0.06,'square',0.35],[720,0.08,'square',0.30]]),
        land:    () => SFX.tone(140, 0.05, 'triangle', 0.35),
        stomp:   () => SFX.sequence([[260,0.05,'square',0.55],[120,0.10,'sawtooth',0.55],[80,0.08,'whitenoise',0.30]]),
        die:     () => SFX.sequence([[440,0.10,'square',0.55],[330,0.12,'sawtooth',0.55],[220,0.18,'sawtooth',0.55],[150,0.30,'triangle',0.55]]),
        win:     () => SFX.sequence([[523,0.10,'square',0.55],[659,0.10,'square',0.60],[784,0.10,'square',0.65],[1047,0.25,'square',0.70]]),
        gameover:() => SFX.sequence([[392,0.20,'sawtooth',0.55],[330,0.20,'sawtooth',0.55],[262,0.40,'triangle',0.55]]),
        timeWarn:() => SFX.tone(880, 0.08, 'square', 0.40),
        flyer:   () => SFX.tone(0,   0.08, 'whitenoise', 0.18),
        beam:    () => SFX.sequence([[1400,0.04,'square',0.35],[800,0.06,'sawtooth',0.45]]),
        boom:    () => SFX.sequence([[120,0.10,'sawtooth',0.65],[60,0.18,'whitenoise',0.55],[40,0.22,'triangle',0.45]]),
        beamHit: () => SFX.tone(180, 0.06, 'whitenoise', 0.50),
        // UI
        menu:    () => SFX.tone(400, 0.04, 'sine',   0.30),
        select:  () => SFX.tone(660, 0.08, 'square', 0.40),
        pause:   () => SFX.sequence([[300,0.05,'square',0.30],[200,0.08,'square',0.30]]),
    };

    // ── Input ────────────────────────────────────────────────────────────────
    Input.init([
        { name: 'left',    label: 'Run Left',  defaults: ['a', 'ArrowLeft'] },
        { name: 'right',   label: 'Run Right', defaults: ['d', 'ArrowRight'] },
        { name: 'primary', label: 'Jump',      defaults: [' ', 'w', 'ArrowUp'] },
        { name: 'shoot',   label: 'Fire Beam', defaults: ['j', 'k', 'f', 'Mouse0'] },
        { name: 'up',      label: 'Menu Up',   defaults: ['ArrowUp'] },
        { name: 'down',    label: 'Menu Down', defaults: ['ArrowDown'] },
        { name: 'confirm', label: 'Confirm',   defaults: ['Enter'] },
        { name: 'pause',   label: 'Pause',     defaults: ['Escape', 'p'] },
    ]);
    Input.attach(window);

    // ── Mouse aim ────────────────────────────────────────────────────────────
    // Raw client coords; virtual-viewport + world coords are resolved at fire
    // / draw time so they always reflect the current camera and letterbox.
    const Mouse = { clientX: 0, clientY: 0, vx: VIEW_W / 2, vy: VIEW_H / 2 };
    function updateMouseFromClient() {
        const rect = canvas.getBoundingClientRect();
        if (!rect || rect.width <= 0 || rect.height <= 0) return;
        const internalW = Canvas.w(ctx, VIEW_W);
        const internalH = Canvas.h(ctx, VIEW_H);
        const cx = (Mouse.clientX - rect.left) * (internalW / rect.width);
        const cy = (Mouse.clientY - rect.top)  * (internalH / rect.height);
        const scale = Math.min(internalW / VIEW_W, internalH / VIEW_H);
        if (scale <= 0) return;
        const ox = Math.floor((internalW - VIEW_W * scale) / 2);
        const oy = Math.floor((internalH - VIEW_H * scale) / 2);
        Mouse.vx = (cx - ox) / scale;
        Mouse.vy = (cy - oy) / scale;
    }
    function aimWorld() {
        updateMouseFromClient();
        return {
            x: Mouse.vx + (Game.cam ? Game.cam.x : 0),
            y: Mouse.vy + (Game.cam ? Game.cam.y : 0),
        };
    }
    window.addEventListener('mousemove', (e) => {
        Mouse.clientX = e.clientX;
        Mouse.clientY = e.clientY;
    });

    // ── Storage ──────────────────────────────────────────────────────────────
    const store = Storage.create('stompworld');
    store.load({ best: 0 });

    // ── Game state ───────────────────────────────────────────────────────────
    const Game = {
        tilemap: null,
        cam: null,
        player: null,
        stompers: [],
        flyers: [],
        flag: null,
        pickup: null,                // {x, y, w, h, t}; null after collected
        score: 0,
        lives: 3,
        timeLeft: 300,
        runAccum: 0,    // for animating run frames
        deathTimer: 0,  // > 0 = death anim playing
        winTimer:   0,  // > 0 = win anim playing
        spawn: { x: 0, y: 0 },
        // Beam weapon — pickup-gated. Player walks unarmed until they
        // collect the beam canister at col 115, after which they can fire
        // mouse-aimed beams that carve destructible terrain. Killed enemies
        // stay in arrays with ragdoll=true so collision skips them and the
        // renderer draws them tumbling.
        hasWeapon: false,
        weaponCooldown: 0,           // ms until next shot allowed
        beams: [],                   // [{x0,y0,x1,y1,ttl,ttlMax}]
        explosions: [],              // [{cx,cy,rMax,ttl,ttlMax}]
        pickupAnimT: 0,              // bob phase for the pickup sprite
        // Tunables.
        BEAM_THICKNESS: 8,
        BEAM_LENGTH:    600,
        EXPLOSION_R:    56,
        WEAPON_COOLDOWN_MS: 250,
        BEAM_TTL_MS:    80,
        EXPLOSION_TTL_MS: 320,
        // Score values (per event).
        SCORE_PER_PIXEL:   0.05,     // ~50 per fully-cleared tile
        SCORE_STOMP:       100,
        SCORE_BEAM_STOMP:  100,
        SCORE_BEAM_FLYER:  500,      // highest per-event reward
        SCORE_PICKUP:      300,
        SCORE_FLAG:        1000,

        loadLevel() {
            const lvl = Level.buildLevel({ tileSize: TILE, destructible: true });
            this.tilemap  = lvl.tilemap;
            this.stompers = lvl.stompers;
            this.flyers   = lvl.flyers;
            this.flag     = lvl.flag;
            this.pickup   = lvl.pickup;
            this.spawn.x  = lvl.spawn.x;
            this.spawn.y  = lvl.spawn.y;

            this.player = Platformer.createBody({
                x: this.spawn.x, y: this.spawn.y - 4,
                w: 24, h: 30,
                cfg: {
                    gravity:    2400,
                    maxFall:    900,
                    runSpeed:   240,
                    accel:      1800,
                    airAccel:   1200,
                    friction:   1800,
                    jumpVel:    -850,
                    jumpCutMul: 0.45,
                    coyoteTime: 100,
                    jumpBuffer: 120,
                },
            });
            this.player.facing = 1;

            this.cam = Camera2D.create({
                viewW: VIEW_W, viewH: VIEW_H,
                levelW: this.tilemap.widthPx,
                levelH: this.tilemap.heightPx,
                deadzoneW: 120, deadzoneH: 1024, // vertical: huge → no vertical scroll
            });
            this.cam.snapTo(this.player.x + this.player.w / 2, VIEW_H / 2);
        },

        respawnPlayer() {
            this.player.x = this.spawn.x;
            this.player.y = this.spawn.y - 4;
            this.player.vx = 0; this.player.vy = 0;
            this.player.coyote = 0; this.player.buffer = 0;
            this.player.facing = 1;
            // Rebuild mob + pickup layout from the level definition so
            // squashed stompers and a collected pickup come back when the
            // player respawns. Reuse the existing arrays so the renderer's
            // references stay stable.
            const lvl = Level.buildLevel({ tileSize: TILE });
            this.stompers.length = 0;
            for (const s of lvl.stompers) this.stompers.push(s);
            this.flyers.length = 0;
            for (const f of lvl.flyers) this.flyers.push(f);
            // Play mode starts armed; the pickup is a training-only gate.
            this.pickup = null;
            this.hasWeapon = true;
            this.weaponCooldown = 0;
            this.cam.snapTo(this.player.x + this.player.w / 2, VIEW_H / 2);
        },

        startRun() {
            this.score = 0; this.lives = 3; this.timeLeft = 300;
            this.deathTimer = 0; this.winTimer = 0;
            this.hasWeapon = true;     // play mode starts armed (pickup is a training-only gate)
            this.weaponCooldown = 0;
            this.beams.length = 0;
            this.explosions.length = 0;
            this.loadLevel();
            this.pickup = null;        // hide the training pickup in play mode
            // Consume any rising-edge shoot input left over from the menu
            // click that started the run, otherwise we'd fire on spawn frame.
            Input.pressed('shoot');
            updateHud();
        },
    };

    // ── HUD ──────────────────────────────────────────────────────────────────
    const hudScore = document.getElementById('hud-score');
    const hudLives = document.getElementById('hud-lives');
    const hudTime  = document.getElementById('hud-time');
    const hudBeam  = document.getElementById('hud-beam');
    function updateHud() {
        hudScore.textContent = Game.score;
        hudLives.textContent = Game.lives;
        hudTime.textContent  = Math.max(0, Math.ceil(Game.timeLeft));
        if (hudBeam) hudBeam.style.display = Game.hasWeapon ? '' : 'none';
    }

    // ── Stomper update ──────────────────────────────────────────────────────
    // Tile-aware AABB sweep with gravity. Reverses on wall hits and at ledge
    // edges, so stompers patrol whatever platform they spawn on.
    const STOMP_GRAVITY = 1800;
    const STOMP_MAX_FALL = 800;
    function stompMoveX(s, dx, tm) {
        s.x += dx;
        const r0 = Math.floor(s.y / TILE);
        const r1 = Math.floor((s.y + s.h - 0.001) / TILE);
        if (dx > 0) {
            const col = Math.floor((s.x + s.w - 0.001) / TILE);
            for (let r = r0; r <= r1; r++) {
                if (tm.solidAt(col, r)) {
                    s.x = col * TILE - s.w; s.vx = -Math.abs(s.vx); return true;
                }
            }
        } else if (dx < 0) {
            const col = Math.floor(s.x / TILE);
            for (let r = r0; r <= r1; r++) {
                if (tm.solidAt(col, r)) {
                    s.x = (col + 1) * TILE; s.vx = Math.abs(s.vx); return true;
                }
            }
        }
        return false;
    }
    function stompMoveY(s, dy, tm) {
        s.y += dy;
        const c0 = Math.floor(s.x / TILE);
        const c1 = Math.floor((s.x + s.w - 0.001) / TILE);
        if (dy > 0) {
            const row = Math.floor((s.y + s.h - 0.001) / TILE);
            for (let c = c0; c <= c1; c++) {
                if (tm.solidAt(c, row)) {
                    s.y = row * TILE - s.h; s.vy = 0; s.onGround = true; return;
                }
            }
        } else if (dy < 0) {
            const row = Math.floor(s.y / TILE);
            for (let c = c0; c <= c1; c++) {
                if (tm.solidAt(c, row)) {
                    s.y = (row + 1) * TILE; s.vy = 0; return;
                }
            }
        }
    }

    function stepStomper(s, dt) {
        if (s.ragdoll) { stepRagdoll(s, dt); return; }
        if (!s.alive) {
            s.squashTimer -= dt;
            return;
        }
        s.animT += dt;
        const tm = Game.tilemap;
        const dts = dt / 1000;

        s.vy += STOMP_GRAVITY * dts;
        if (s.vy > STOMP_MAX_FALL) s.vy = STOMP_MAX_FALL;

        s.onGround = false;
        stompMoveX(s, s.vx * dts, tm);
        stompMoveY(s, s.vy * dts, tm);

        // Don't walk off ledges: if grounded and the tile under the leading foot
        // is empty, flip direction. Stompers fall off conveyer-style only when
        // gravity carries them off (e.g. squashed mid-air or off a moving platform).
        if (s.onGround) {
            const probeX = s.vx > 0 ? s.x + s.w + 1 : s.x - 1;
            const probeY = s.y + s.h + 2;
            if (!tm.solidAtPx(probeX, probeY)) s.vx = -s.vx;
        }
    }

    // ── Flyer step ──────────────────────────────────────────────────────────
    // Sky enemies that patrol horizontally and (optionally) bob vertically.
    // No tilemap collision; they live above the ground.
    function stepFlyer(f, dt) {
        if (f.ragdoll) { stepRagdoll(f, dt); return; }
        const dts = dt / 1000;
        f.x += f.vx * dts;
        if (f.x > f.spawnX + f.patrolRange) {
            f.x = f.spawnX + f.patrolRange;
            f.vx = -Math.abs(f.vx);
        } else if (f.x < f.spawnX - f.patrolRange) {
            f.x = f.spawnX - f.patrolRange;
            f.vx = Math.abs(f.vx);
        }
        if (f.bobAmp > 0) {
            f.bobT += dts;
            const newY = f.spawnY + Math.sin(f.bobT * f.bobFreq) * f.bobAmp;
            f.vy = (newY - f.y) / dts;
            f.y = newY;
        } else {
            f.vy = 0;
        }
        f.animT += dt;
    }
    function handleFlyers() {
        const p = Game.player;
        for (const f of Game.flyers) {
            if (f.ragdoll) continue;
            if (p.x + p.w <= f.x || p.x >= f.x + f.w) continue;
            if (p.y + p.h <= f.y || p.y >= f.y + f.h) continue;
            // Any contact kills — flyers are not stompable.
            killPlayer();
            return;
        }
    }

    // ── Player ↔ stomper ────────────────────────────────────────────────────
    function handleStompers() {
        const p = Game.player;
        for (const s of Game.stompers) {
            if (!s.alive || s.ragdoll) continue;
            if (p.x + p.w <= s.x || p.x >= s.x + s.w) continue;
            if (p.y + p.h <= s.y || p.y >= s.y + s.h) continue;
            // From above (falling and feet near stomper top) = stomp.
            const fromAbove = p.vy > 0 && (p.y + p.h - s.y) < 16;
            if (fromAbove) {
                s.alive = false;
                s.squashTimer = 350;
                p.vy = -380;
                Game.score += Game.SCORE_STOMP;
                Audio.stomp();
                updateHud();
            } else {
                killPlayer();
                return;
            }
        }
    }

    function killPlayer() {
        if (Game.deathTimer > 0) return;
        Game.lives--;
        Audio.die();
        Game.deathTimer = 900;
        updateHud();
    }

    // ── Weapon: beam + explosion + ragdoll launches ─────────────────────────
    // Beam fires horizontal from player center in facing direction. damageBeam
    // walks the centerline pixel-by-pixel (stopOnHit=true), carving an 8px
    // band up to the impact point; we then carve a 56px disc explosion there.
    // Anything (flyer/stomper) AABB-overlapping the beam path or the explosion
    // disc gets ragdoll-launched.
    function rdRand(min, max) { return min + Math.random() * (max - min); }

    function ragdollify(e, dirX) {
        e.alive   = false;
        e.ragdoll = true;
        e.vx      = dirX * rdRand(180, 380) + rdRand(-60, 60);
        e.vy      = -rdRand(420, 720);
        e.rot     = 0;
        e.rotVel  = rdRand(-12, 12);
        e.ragdollTTL = 2200;
    }

    // Test if AABB e overlaps either the beam segment thickened to ±half px,
    // or the explosion disc at (hx, hy) of radius r. The beam is given as a
    // line from (x0,y0) to (x1,y1) at any angle. We use Liang–Barsky against
    // the AABB expanded by `half` on every side as a conservative band test —
    // it slightly over-reports hits at the band's outer corners, which is
    // fine here (the beam visual already has rounded caps).
    function entityHit(e, x0, y0, x1, y1, half, hx, hy, r) {
        const ex0 = e.x, ex1 = e.x + e.w;
        const ey0 = e.y, ey1 = e.y + e.h;
        // Explosion disc.
        const cx = hx < ex0 ? ex0 : (hx > ex1 ? ex1 : hx);
        const cy = hy < ey0 ? ey0 : (hy > ey1 ? ey1 : hy);
        const ddx = cx - hx, ddy = cy - hy;
        if (ddx * ddx + ddy * ddy <= r * r) return true;
        // Beam segment vs expanded AABB.
        const ax0 = ex0 - half, ay0 = ey0 - half;
        const ax1 = ex1 + half, ay1 = ey1 + half;
        const dx = x1 - x0, dy = y1 - y0;
        const ps = [-dx, dx, -dy, dy];
        const qs = [x0 - ax0, ax1 - x0, y0 - ay0, ay1 - y0];
        let t0 = 0, t1 = 1;
        for (let i = 0; i < 4; i++) {
            if (ps[i] === 0) {
                if (qs[i] < 0) return false;
            } else {
                const t = qs[i] / ps[i];
                if (ps[i] < 0) {
                    if (t > t1) return false;
                    if (t > t0) t0 = t;
                } else {
                    if (t < t0) return false;
                    if (t < t1) t1 = t;
                }
            }
        }
        return true;
    }

    function fireWeapon() {
        if (!Game.hasWeapon || Game.weaponCooldown > 0) return;
        const p = Game.player;
        const px = p.x + p.w / 2;
        const py = p.y + p.h / 2;
        // Aim direction: from player center toward the cursor's world point.
        const aim = aimWorld();
        let dxA = aim.x - px, dyA = aim.y - py;
        const dist = Math.hypot(dxA, dyA);
        let ux, uy;
        if (dist < 1) {
            ux = p.facing < 0 ? -1 : 1; uy = 0;
        } else {
            ux = dxA / dist; uy = dyA / dist;
        }
        // Face the way we're shooting so the run/idle sprite reads correctly.
        if (Math.abs(ux) > 0.05) p.facing = ux < 0 ? -1 : 1;
        // Start the beam outside the player AABB along the aim direction so
        // we don't immediately stop on a tile the player is overlapping.
        const startOff = p.w / 2 + 2;
        const x0 = px + ux * startOff;
        const y0 = py + uy * startOff;
        const x1 = px + ux * Game.BEAM_LENGTH;
        const y1 = py + uy * Game.BEAM_LENGTH;
        const r = Game.tilemap.damageBeam(x0, y0, x1, y1, Game.BEAM_THICKNESS, true);
        const hx = r.hitX, hy = r.hitY;
        const explosionR = r.hit ? Game.EXPLOSION_R : 0;
        let pixelsCleared = r.cleared | 0;
        if (explosionR > 0) {
            pixelsCleared += Game.tilemap.damageCircle(hx, hy, explosionR) | 0;
        }
        if (pixelsCleared > 0) {
            Game.score += Math.floor(pixelsCleared * Game.SCORE_PER_PIXEL);
        }

        // Visuals.
        Game.beams.push({
            x0, y0, x1: hx, y1: hy,
            ttl: Game.BEAM_TTL_MS, ttlMax: Game.BEAM_TTL_MS,
        });
        if (explosionR > 0) {
            Game.explosions.push({
                cx: hx, cy: hy, rMax: explosionR,
                ttl: Game.EXPLOSION_TTL_MS, ttlMax: Game.EXPLOSION_TTL_MS,
            });
        }

        // Hits. Ragdoll launch direction uses the beam's horizontal component
        // so enemies tumble away from the shooter regardless of aim angle.
        const halfBeam = Game.BEAM_THICKNESS / 2 + 2;
        const launchDir = ux < 0 ? -1 : 1;
        let killedAny = false;
        for (const f of Game.flyers) {
            if (f.ragdoll) continue;
            if (entityHit(f, x0, y0, hx, hy, halfBeam, hx, hy, explosionR)) {
                ragdollify(f, launchDir); killedAny = true;
                Game.score += Game.SCORE_BEAM_FLYER;
            }
        }
        for (const s of Game.stompers) {
            if (!s.alive || s.ragdoll) continue;
            if (entityHit(s, x0, y0, hx, hy, halfBeam, hx, hy, explosionR)) {
                ragdollify(s, launchDir); killedAny = true;
                Game.score += Game.SCORE_BEAM_STOMP;
            }
        }
        if (killedAny || pixelsCleared > 0) updateHud();

        Audio.beam();
        if (r.hit) Audio.boom();
        Game.weaponCooldown = Game.WEAPON_COOLDOWN_MS;
    }

    function stepRagdoll(e, dt) {
        const dts = dt / 1000;
        e.vy += 1800 * dts;            // gravity
        e.x  += e.vx * dts;
        e.y  += e.vy * dts;
        e.rot = (e.rot || 0) + (e.rotVel || 0) * dts;
        e.ragdollTTL -= dt;
    }

    function pruneRagdolls() {
        const tm = Game.tilemap;
        const cap = tm ? tm.heightPx + 200 : 9999;
        function alive(e) { return !e.ragdoll || (e.ragdollTTL > 0 && e.y < cap); }
        Game.flyers   = Game.flyers.filter(alive);
        Game.stompers = Game.stompers.filter(alive);
    }

    function updateBeamsExplosions(dt) {
        for (const b of Game.beams) b.ttl -= dt;
        for (const e of Game.explosions) e.ttl -= dt;
        if (Game.beams.length)
            Game.beams = Game.beams.filter((b) => b.ttl > 0);
        if (Game.explosions.length)
            Game.explosions = Game.explosions.filter((e) => e.ttl > 0);
    }

    // Beam pickup: AABB overlap arms the player and removes the pickup.
    function checkPickup() {
        if (!Game.pickup || Game.hasWeapon) return;
        const p = Game.player;
        const pk = Game.pickup;
        if (p.x + p.w <= pk.x || p.x >= pk.x + pk.w) return;
        if (p.y + p.h <= pk.y || p.y >= pk.y + pk.h) return;
        Game.hasWeapon = true;
        Game.pickup = null;
        Game.score += Game.SCORE_PICKUP;
        Audio.select();
        updateHud();
    }

    // ── Win / lose check ─────────────────────────────────────────────────────
    function checkWinLose() {
        const p = Game.player;
        // Fall out of the world.
        if (p.y > Game.tilemap.heightPx + 64) {
            killPlayer();
            return;
        }
        // Touch flag.
        if (Game.flag && Game.winTimer === 0) {
            const f = Game.flag;
            if (p.x + p.w >= f.x + 8 && p.x <= f.x + f.w - 8) {
                Game.score += Game.SCORE_FLAG;
                Audio.win();
                Game.winTimer = 1500;
                updateHud();
            }
        }
    }

    // ── Render ───────────────────────────────────────────────────────────────
    function drawSky() {
        const g = ctx.createLinearGradient(0, 0, 0, VIEW_H);
        g.addColorStop(0, '#6cb0f0');
        g.addColorStop(1, '#a8d4f8');
        ctx.fillStyle = g;
        ctx.fillRect(0, 0, VIEW_W, VIEW_H);
    }

    function drawHero() {
        const p = Game.player;
        let frame = 0;
        if (!p.onGround) {
            frame = 3;
        } else if (Math.abs(p.vx) > 8) {
            Game.runAccum += Math.abs(p.vx) * 0.0006;
            frame = 1 + (Math.floor(Game.runAccum) % 2);
        }
        Art.drawHero(ctx,
                     p.x - Game.cam.x,
                     p.y - Game.cam.y - 2,
                     frame, p.facing < 0);
    }

    function drawStompers() {
        for (const s of Game.stompers) {
            if (!Game.cam.visible(s.x, s.y, s.w, s.h)) continue;
            const frame = !s.alive ? 2 : (Math.floor(s.animT / 200) % 2);
            const dx = s.x - Game.cam.x;
            const dy = s.y - Game.cam.y;
            if (s.ragdoll) {
                ctx.save();
                ctx.translate(dx + s.w / 2, dy + s.h / 2);
                ctx.rotate(s.rot || 0);
                Art.drawStomper(ctx, -s.w / 2, -s.h / 2, 0);
                ctx.restore();
            } else {
                Art.drawStomper(ctx, dx, dy, frame);
            }
        }
    }

    function drawFlyers() {
        for (const f of Game.flyers) {
            if (!Game.cam.visible(f.x, f.y, f.w, f.h)) continue;
            const frame = (Math.floor(f.animT / 150) % 2);
            const dx = f.x - Game.cam.x;
            const dy = f.y - Game.cam.y;
            if (f.ragdoll) {
                ctx.save();
                ctx.translate(dx + f.w / 2, dy + f.h / 2);
                ctx.rotate(f.rot || 0);
                Art.drawFlyer(ctx, -f.w / 2, -f.h / 2, frame, false);
                ctx.restore();
            } else {
                Art.drawFlyer(ctx, dx, dy, frame, f.vx > 0);
            }
        }
    }

    function drawBeamsExplosions() {
        // Beam: bright outer band + white-hot core, both fading by ttl.
        for (const b of Game.beams) {
            const a = Math.max(0, b.ttl / b.ttlMax);
            ctx.save();
            ctx.lineCap = 'round';
            ctx.strokeStyle = 'rgba(255, 220, 80, ' + (a * 0.85).toFixed(3) + ')';
            ctx.lineWidth = Game.BEAM_THICKNESS + 6;
            ctx.beginPath();
            ctx.moveTo(b.x0 - Game.cam.x, b.y0 - Game.cam.y);
            ctx.lineTo(b.x1 - Game.cam.x, b.y1 - Game.cam.y);
            ctx.stroke();
            ctx.strokeStyle = 'rgba(255, 255, 240, ' + a.toFixed(3) + ')';
            ctx.lineWidth = Game.BEAM_THICKNESS - 4;
            ctx.beginPath();
            ctx.moveTo(b.x0 - Game.cam.x, b.y0 - Game.cam.y);
            ctx.lineTo(b.x1 - Game.cam.x, b.y1 - Game.cam.y);
            ctx.stroke();
            ctx.restore();
        }
        // Explosion: expanding orange outer + white core, both fading.
        for (const e of Game.explosions) {
            const u = 1 - Math.max(0, e.ttl / e.ttlMax);     // 0 → 1
            const r = e.rMax * (0.35 + 0.65 * u);
            const a = (1 - u) * 0.95;
            const cx = e.cx - Game.cam.x;
            const cy = e.cy - Game.cam.y;
            ctx.save();
            ctx.fillStyle = 'rgba(255, 150, 40, ' + a.toFixed(3) + ')';
            ctx.beginPath();
            ctx.arc(cx, cy, r, 0, Math.PI * 2);
            ctx.fill();
            ctx.fillStyle = 'rgba(255, 240, 200, ' + (a * 0.7).toFixed(3) + ')';
            ctx.beginPath();
            ctx.arc(cx, cy, r * 0.55, 0, Math.PI * 2);
            ctx.fill();
            ctx.restore();
        }
    }

    function drawAimCursor() {
        updateMouseFromClient();
        const x = Mouse.vx, y = Mouse.vy;
        if (x < 0 || x > VIEW_W || y < 0 || y > VIEW_H) return;
        ctx.save();
        ctx.strokeStyle = 'rgba(255, 240, 80, 0.85)';
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.moveTo(x - 6, y); ctx.lineTo(x - 2, y);
        ctx.moveTo(x + 2, y); ctx.lineTo(x + 6, y);
        ctx.moveTo(x, y - 6); ctx.lineTo(x, y - 2);
        ctx.moveTo(x, y + 2); ctx.lineTo(x, y + 6);
        ctx.stroke();
        ctx.restore();
    }

    function drawFlag() {
        if (!Game.flag) return;
        const f = Game.flag;
        Art.drawFlag(ctx, f.x - Game.cam.x, f.y - Game.cam.y);
    }

    function drawPickup() {
        if (!Game.pickup) return;
        const pk = Game.pickup;
        Art.drawPickup(ctx,
            pk.x - Game.cam.x,
            pk.y - Game.cam.y,
            Game.pickupAnimT);
    }

    // Letterbox the fixed VIEW_W × VIEW_H virtual viewport into the actual
    // canvas surface (which the bro engine sizes from the CSS layout box, not
    // from canvas.width). Aspect is preserved; bars are drawn black.
    function draw() {
        const W = Canvas.w(ctx, VIEW_W);
        const H = Canvas.h(ctx, VIEW_H);
        ctx.save();
        ctx.fillStyle = '#000';
        ctx.fillRect(0, 0, W, H);
        const scale = Math.min(W / VIEW_W, H / VIEW_H);
        const ox = Math.floor((W - VIEW_W * scale) / 2);
        const oy = Math.floor((H - VIEW_H * scale) / 2);
        ctx.translate(ox, oy);
        ctx.scale(scale, scale);
        // Clip so out-of-viewport content (over-wide tilemaps, off-screen sprites)
        // doesn't bleed into the letterbox bars.
        ctx.beginPath();
        ctx.rect(0, 0, VIEW_W, VIEW_H);
        ctx.clip();

        drawSky();
        if (S.name() === 'training' && Training.lvlTilemap) {
            Training.draw();
        } else if (S.name() === 'demo') {
            demoCtl.draw();
        } else if (Game.tilemap) {
            Game.tilemap.draw(ctx, Game.cam.x, Game.cam.y, VIEW_W, VIEW_H);
            drawFlag();
            drawPickup();
            drawStompers();
            drawFlyers();
            drawHero();
            drawBeamsExplosions();
            if (Game.hasWeapon) drawAimCursor();
        }
        ctx.restore();
    }

    // ── Update ───────────────────────────────────────────────────────────────
    function update(dt) {
        if (S.name() === 'training') { Training.update(dt); return; }
        if (S.name() === 'demo')     { demoCtl.update(dt);  return; }
        if (S.name() !== 'playing') return;

        // Tick visuals + ragdolls every frame regardless of win/death anim
        // so they don't linger across screen transitions.
        updateBeamsExplosions(dt);
        pruneRagdolls();

        // Win celebration: drift right, ignore input, then advance.
        if (Game.winTimer > 0) {
            Game.winTimer -= dt;
            Game.player.vx = 80;
            Platformer.step(Game.player, { right: true }, Game.tilemap, dt);
            for (const s of Game.stompers) stepStomper(s, dt);
            for (const f of Game.flyers)   stepFlyer(f, dt);
            Game.cam.follow(Game.player.x + Game.player.w / 2, VIEW_H / 2);
            if (Game.winTimer <= 0) {
                if (Game.score > store.get('best')) {
                    store.set('best', Game.score); store.save();
                }
                S.switchTo('win');
            }
            return;
        }

        // Death animation: hop and fall, no input.
        if (Game.deathTimer > 0) {
            Game.deathTimer -= dt;
            Game.player.vy += 2400 * (dt / 1000);
            Game.player.y  += Game.player.vy * (dt / 1000);
            if (Game.deathTimer <= 0) {
                if (Game.lives <= 0) {
                    if (Game.score > store.get('best')) {
                        store.set('best', Game.score); store.save();
                    }
                    Audio.gameover();
                    S.switchTo('gameover');
                } else {
                    Game.respawnPlayer();
                    Game.deathTimer = 0;
                }
            }
            return;
        }

        // Time tick.
        const prevTime = Game.timeLeft;
        Game.timeLeft -= dt / 1000;
        if (Game.timeLeft <= 0) { killPlayer(); updateHud(); return; }
        const tNow = Math.floor(Game.timeLeft);
        if (tNow !== Math.floor(prevTime)) {
            updateHud();
            if (tNow <= 5 && tNow >= 1) Audio.timeWarn();
        }

        // Player physics.
        const pev = Platformer.step(Game.player, {
            left:        Input.down('left'),
            right:       Input.down('right'),
            jumpHeld:    Input.down('primary'),
            jumpPressed: Input.pressed('primary'),
        }, Game.tilemap, dt);
        if (pev.jumped) Audio.jump();
        if (pev.landed) Audio.land();

        if (Input.pressed('pause')) { Audio.pause(); S.switchTo('pause'); return; }

        // Weapon: cooldown decay + fire on shoot edge.
        if (Game.weaponCooldown > 0) Game.weaponCooldown -= dt;
        if (Input.pressed('shoot')) fireWeapon();

        for (const s of Game.stompers) stepStomper(s, dt);
        for (const f of Game.flyers)   stepFlyer(f, dt);
        handleStompers();
        handleFlyers();
        checkPickup();
        checkWinLose();
        Game.pickupAnimT += dt;

        Game.cam.follow(Game.player.x + Game.player.w / 2, VIEW_H / 2);
    }

    // ── Training mode ────────────────────────────────────────────────────────
    // The visualizer is a *replay* of recently-completed trajectories, not a
    // live agent. Training itself runs entirely in workers:
    //
    //   trainer_worker      owns net + buffer + SGD; ingests tuples from the
    //                       mcts workers, publishes weights, persists ckpts.
    //   mcts_worker × N     self-play data generators at varying search
    //                       depths. Each posts {tuples, trajectory} per
    //                       episode end.
    //
    // Main thread:
    //   - routes tuples + trajectory_end to the trainer; broadcasts weights
    //     back to the mcts workers.
    //   - keeps a ring of the last 10 trajectories. Each playback cycle:
    //       primary  = best-by-return of the latest 10 (replayed by stepping
    //                  this thread's sim from the trajectory's startSnap +
    //                  actions, full world state).
    //       ghosts   = the other 9 trajectories' player tracks (extracted
    //                  once on selection by replaying through a throwaway
    //                  sim and capturing per-tick {x, y, facing, frame}),
    //                  rendered as translucent hero sprites only.
    //     Tier: ≥10 trajectories → 1 primary + 9 ghosts; ≥5 → 1 + 4; <5 → 1
    //     (just the primary, no ghosts).
    //   - steps the primary sim at 60 Hz (FIXED_DT_MS = 16.67 ms) on the main
    //     thread — one wall-clock tick per call to sim.tickPhysics — so the
    //     world moves continuously instead of jumping per decision.
    //
    // Controls: F = fast (8× playback), C = clear failure tapes on workers,
    //           Esc = back to title.
    const Training = {
        trainer: null,
        mctsWorkers: [],
        pool: null,
        cam: null,
        sim: null,                    // main-thread sim, stepped during primary playback
        fast: false,
        FAST_MULT: 8,
        running: false,

        // Level data shared with this.sim — tilemap mutations from the sim's
        // auto-fire pass appear directly on the rendered world. Templates
        // (lvlStompers, lvlFlyers) are unused at draw time now that the sim
        // owns mob state, but kept for reference.
        lvlTilemap: null,
        lvlFlag: null,
        lvlPickup: null,

        // Worker-config knobs. Two MCTS data workers at very different
        // search depths so the trainer sees a mix of cheap/diverse and
        // deep/confident visit distributions.
        NUM_MCTS_WORKERS: 5,
        MCTS_DEPTHS:  [40, 80, 100, 100, 100],
        MCTS_ROLLOUT: [4, 4, 4, 6, 10],

        // ── Replay state ─────────────────────────────────────────────────
        // Ring buffer of the last N trajectories from any worker. We
        // re-select the primary + ghost set on each playback boundary,
        // not on each new trajectory arrival, so a run plays through to
        // its end before we swap.
        TRAJ_RING_SIZE: 10,
        trajRing: [],
        primary: null,                // currently-playing trajectory
        actionIdx: 0,                 // index into primary.actions
        tickInDecision: 0,            // 0..FRAME_SKIP-1 within current decision
        tickAcc: 0,                   // wall-clock ms accumulator for 60 Hz physics
        primaryDone: false,           // playback exhausted; next update() re-selects
        primaryStartTick: 0,          // sim.tick at start of current playback
        episodesPlayed: 0,            // how many primaries we've shown
        ghostTracks: [],              // [{frames: [{tick,x,y,facing,frame}]}, ...]

        // Audio diff state — compare prev/current sim state at each
        // decision boundary. Mute in fast mode so 8× playback doesn't
        // machine-gun the speakers.
        prevPlayer: null,
        prevStomperAlive: null,
        flyerCooldown: 0,

        // HUD aggregates.
        workerStats: {
            ingested: 0, bufSize: 0, trainSteps: 0,
            lossValue: 0, lossPolicy: 0,
            netVersion: 0n, bestMean: 0, meanReturn: 0, resumed: 0,
        },
        // Beam visuals fired by the sim on its auto-fire passes; decayed
        // by ttl across the next several frames so a single shot stays
        // visible regardless of where in the playback we are.
        trainingBeams: [],
        mctsStats: [],
        warmupInfo: null,
        poolTopReturn: 0,
        poolCapacity: 32,

        start() {
            // The level provides the tilemap, mob templates, flag and pickup.
            // We hand the same tilemap to the sim so its auto-fire damage
            // shows up on the rendered world directly — no diff protocol.
            const lvl = Level.buildLevel({ tileSize: TILE, destructible: true });
            this.lvlTilemap = lvl.tilemap;
            this.lvlFlag    = lvl.flag;
            this.lvlPickup  = lvl.pickup;
            this.pickupAnimT = 0;

            this.sim = SwSim.create({
                tilemap: lvl.tilemap, spawn: lvl.spawn,
                stompers: lvl.stompers, flyers: lvl.flyers,
                flag: lvl.flag, pickup: lvl.pickup,
                timeLimit: 600,
            });

            this.cam = Camera2D.create({
                viewW: VIEW_W, viewH: VIEW_H,
                levelW: lvl.tilemap.widthPx,
                levelH: lvl.tilemap.heightPx,
                deadzoneW: 120, deadzoneH: 1024,
            });
            this.cam.snapTo(VIEW_W / 2, VIEW_H / 2);

            this.trajRing = [];
            this.primary = null;
            this.actionIdx = 0;
            this.tickInDecision = 0;
            this.tickAcc = 0;
            this.primaryDone = false;
            this.primaryStartTick = 0;
            this.episodesPlayed = 0;
            this.ghostTracks = [];

            this.prevPlayer = null;
            this.prevStomperAlive = null;
            this.flyerCooldown = 0;

            this.pool = bro.ai.game.grid.createBestCrop({
                capacity: this.poolCapacity,
                depthBonus: 0.001, ageDecay: 0.0001,
                seedTopK: 8, seed: 0xC0DE5EEDn,
            });
            this.poolTopReturn = 0;
            this.poolAccepted = 0;
            this.fast = false;
            this.warmupInfo = null;
            this.trainingBeams = [];
            this.mctsStats = [];
            for (let i = 0; i < this.NUM_MCTS_WORKERS; i++) this.mctsStats.push({});

            // Spawn workers up front. They run concurrently with the
            // trainer's synchronous BC warmup. A trainerReady gate in
            // routeTuples / ingestTrajectory drops tuples on main during
            // warmup so they don't pile up in the trainer's inbox holding
            // Float32Arrays alive while it's busy with pretrain.
            this.lastWeightsBytes   = null;
            this.lastWeightsVersion = 0n;
            this.trainerReady       = false;
            this.droppedTuples      = 0;
            this.trainer = new Worker('trainer_worker.js');
            this.trainer.onmessage = (e) => this.onTrainerMessage(e && e.data);

            this.mctsWorkers = [];
            for (let i = 0; i < this.NUM_MCTS_WORKERS; i++) {
                const w = new Worker('mcts_worker.js');
                const idx = i;
                w.onmessage = (e) => this.onMctsMessage(e && e.data, idx);
                w.postMessage({
                    type: 'init',
                    workerId:     idx + 1,
                    iterations:   this.MCTS_DEPTHS[i]  || 100,
                    rolloutDepth: this.MCTS_ROLLOUT[i] || 8,
                });
                this.mctsWorkers.push(w);
            }
            this.running = true;
        },

        stop() {
            this.running = false;
            const all = [this.trainer, ...(this.mctsWorkers || [])].filter(Boolean);
            for (const w of all) {
                try { w.postMessage({ type: 'stop' }); } catch (_) {}
                try { w.terminate(); } catch (_) {}
            }
            this.trainer = null;
            this.mctsWorkers = [];
        },

        // Each recipient needs its own ArrayBuffer so we can hand them off
        // zero-copy via the transferList. Cloning the small weights blob N
        // times costs less than the cross-thread copy postMessage would
        // otherwise do for N recipients.
        sendWeightsTo(worker, bytes, version) {
            const copy = new Uint8Array(bytes.length);
            copy.set(bytes);
            try {
                worker.postMessage({
                    type: 'weights', bytes: copy, version,
                }, [copy.buffer]);
            } catch (_) {}
        },
        broadcastWeights(bytes, version) {
            for (const r of this.mctsWorkers) {
                if (r) this.sendWeightsTo(r, bytes, version);
            }
        },

        onTrainerMessage(m) {
            if (!m) return;
            if (m.type === 'weights') {
                this.lastWeightsBytes   = m.bytes;
                this.lastWeightsVersion = m.version;
                this.broadcastWeights(m.bytes, m.version);
                if (m.stats) Object.assign(this.workerStats, m.stats);
            } else if (m.type === 'stats') {
                if (m.stats) Object.assign(this.workerStats, m.stats);
            } else if (m.type === 'warmup') {
                this.warmupInfo = m.stats || {};
                // Trainer is responsive now — start streaming tuples through.
                // Anything produced during warmup was dropped on main to keep
                // the trainer's inbox empty.
                this.trainerReady = true;
            }
        },

        onMctsMessage(m, idx) {
            if (!m) return;
            if (m.type === 'tuples') {
                this.routeTuples(m);
            } else if (m.type === 'trajectory') {
                this.ingestTrajectory(m);
            } else if (m.type === 'tape_record') {
                // Fan out to every other mcts worker so they all share a
                // single union tape. Source already recorded its own copy.
                for (let i = 0; i < this.mctsWorkers.length; i++) {
                    if (i === idx) continue;
                    const w = this.mctsWorkers[i];
                    if (!w) continue;
                    try {
                        w.postMessage({ type: 'tape_apply', trace: m.trace });
                    } catch (_) {}
                }
            } else if (m.type === 'stats') {
                this.mctsStats[idx] = m;
            } else if (m.type === 'ready') {
                // Worker finished a batch and is waiting for the next.
                // Send another 'tick' to keep it fed. This is back-pressure:
                // we never queue more than ~1 batch ahead, so a stall in
                // any worker doesn't pile up.
                if (this.mctsWorkers[idx]) {
                    try {
                        this.mctsWorkers[idx].postMessage({ type: 'tick' });
                    } catch (_) {}
                }
            }
        },

        routeTuples(m) {
            if (!this.trainer) return;
            // Drop tuples while the trainer is in synchronous warmup —
            // posting them would just queue in its inbox and pin the
            // tuple Float32Arrays alive until warmup returned. The play
            // workers self-clock and will keep producing fresh tuples
            // once trainerReady flips.
            if (!this.trainerReady) {
                this.droppedTuples += (m.tuples ? m.tuples.length : 0);
                return;
            }
            try {
                this.trainer.postMessage({
                    type: 'tuples',
                    tuples: m.tuples,
                    reason: m.reason,
                    weight: m.weight | 0,
                });
            } catch (_) {}
        },

        ingestTrajectory(m) {
            // BestCrop pool: kept for stats / future use. Ranks by
            // {snapshot, prefix, score, depth} with depthBonus + ageDecay.
            this.pool.push({
                snapshot: m.startSnap,
                prefix:   m.actions,
                score:    m.totalReturn,
                depth:    m.searchDepth,
            });
            this.poolAccepted++;
            if (m.totalReturn > this.poolTopReturn) this.poolTopReturn = m.totalReturn;
            if (this.trainer && this.trainerReady) {
                try {
                    this.trainer.postMessage({
                        type: 'trajectory_end',
                        totalReturn: m.totalReturn,
                        reason: m.reason,
                    });
                } catch (_) {}
            }
            // Visualizer ring: every trajectory enters. All workers spawn at
            // col 2 now, so totalReturn is comparable across workers and the
            // ring rotates fast enough that the primary keeps changing.
            this.trajRing.push({
                startSnap:   m.startSnap,
                actions:     m.actions,
                totalReturn: m.totalReturn,
                reason:      m.reason,
                decisions:   m.decisions,
                bestX:       m.bestX,
                workerId:    m.workerId,
                searchDepth: m.searchDepth,
            });
            while (this.trajRing.length > this.TRAJ_RING_SIZE) this.trajRing.shift();
        },

        // Pick the primary + ghost set for the next playback cycle. Tier:
        //   ≥10 trajectories → window of last 10, primary + 9 ghosts
        //   ≥5              → window of last 5,  primary + 4 ghosts
        //   ≥1              → just the primary  (no ghosts)
        // Primary = highest totalReturn in the window.
        selectPrimaryAndGhosts() {
            const ring = this.trajRing;
            if (ring.length === 0) return null;
            let window;
            if (ring.length >= 10)     window = ring.slice(-10);
            else if (ring.length >= 5) window = ring.slice(-5);
            else                       window = ring.slice(-1);
            let bestIdx = 0;
            for (let i = 1; i < window.length; i++) {
                if (window[i].totalReturn > window[bestIdx].totalReturn) bestIdx = i;
            }
            const ghosts = [];
            for (let i = 0; i < window.length; i++) {
                if (i !== bestIdx) ghosts.push(window[i]);
            }
            return { primary: window[bestIdx], ghosts };
        },

        // Replay a trajectory through a throwaway sim and capture per-tick
        // (x, y, facing, frame). Tick numbers are zero-based at startSnap so
        // they index directly with the primary's relative tick at draw time.
        // Throwaway sim doesn't share the rendered tilemap.
        extractGhostTrack(traj) {
            const lvl = Level.buildLevel({
                tileSize: TILE, destructible: true, trackDamagedTiles: false,
            });
            const tmp = SwSim.create({
                tilemap: lvl.tilemap, spawn: lvl.spawn,
                stompers: lvl.stompers, flyers: lvl.flyers,
                flag: lvl.flag, pickup: lvl.pickup,
                timeLimit: 600,
            });
            tmp.restore(traj.startSnap);
            const frames = [];
            const FS = SwSim.FRAME_SKIP;
            outer: for (let i = 0; i < traj.actions.length; i++) {
                tmp.beginDecision();
                for (let t = 0; t < FS; t++) {
                    const ended = tmp.tickPhysics(traj.actions[i], t);
                    const p = tmp.player;
                    frames.push({
                        tick: i * FS + t,
                        x: p.x, y: p.y, facing: p.facing,
                        frame: heroFrame(p, i * FS + t),
                    });
                    if (ended) { tmp.endDecision(); break outer; }
                }
                const out = tmp.endDecision();
                if (out.done) break;
            }
            return { frames };
        },

        // Set up the sim and ghost tracks for a fresh playback. Resets the
        // shared tilemap's damage state via sim.reset() so the world starts
        // visually clean for the new run.
        startPrimaryPlayback() {
            const sel = this.selectPrimaryAndGhosts();
            if (!sel) return false;
            this.primary = sel.primary;
            this.ghostTracks = sel.ghosts.map((t) => this.extractGhostTrack(t));
            this.actionIdx = 0;
            this.tickInDecision = 0;
            this.tickAcc = 0;
            this.primaryDone = false;
            this.sim.reset();
            this.sim.restore(this.primary.startSnap);
            this.primaryStartTick = this.sim.tick;
            this.sim.beginDecision();
            this.episodesPlayed++;
            this.prevPlayer = null;
            this.prevStomperAlive = this.sim.stompers.map((s) => s.alive);
            this.trainingBeams.length = 0;
            return true;
        },

        update(dt) {
            if (!this.running) return;
            this.pickupAnimT += dt;
            if (this.flyerCooldown > 0) this.flyerCooldown -= dt;

            // No primary yet, or current one is exhausted — try to (re)start.
            if (!this.primary || this.primaryDone) {
                if (!this.startPrimaryPlayback()) {
                    // No trajectories in the ring yet; nothing to play.
                    return;
                }
            }

            // Step the sim at 60 Hz wall-clock (FAST_MULT× in fast mode).
            // Cap the accumulator to avoid spiral-of-death after a hitch.
            const FIXED_DT_MS = SwSim.FIXED_DT_MS;
            const FRAME_SKIP  = SwSim.FRAME_SKIP;
            this.tickAcc += dt * (this.fast ? this.FAST_MULT : 1);
            if (this.tickAcc > 200) this.tickAcc = 200;
            while (this.tickAcc >= FIXED_DT_MS && !this.primaryDone) {
                const action = this.primary.actions[this.actionIdx];
                const ended = this.sim.tickPhysics(action, this.tickInDecision);
                this.tickInDecision++;
                this.tickAcc -= FIXED_DT_MS;
                if (this.tickInDecision >= FRAME_SKIP || ended) {
                    const out = this.sim.endDecision();
                    // Bake the auto-fire damage so it shows on the rendered
                    // tilemap. Capture beam visuals fired this decision and
                    // diff sim state for audio cues. Then advance / wrap.
                    if (this.lvlTilemap.commitOverlays) {
                        this.lvlTilemap.commitOverlays();
                    }
                    for (const b of this.sim.recentBeams) {
                        this.trainingBeams.push({
                            x0: b.x0, y0: b.y0, x1: b.x1, y1: b.y1,
                            ttl: Game.BEAM_TTL_MS, ttlMax: Game.BEAM_TTL_MS,
                        });
                    }
                    this.diffSimAudio();
                    this.tickInDecision = 0;
                    this.actionIdx++;
                    if (out.done || this.actionIdx >= this.primary.actions.length) {
                        this.primaryDone = true;
                        if (!this.fast) {
                            if (this.primary.reason === 'flag')       Audio.win();
                            else if (this.primary.reason === 'death') Audio.die();
                        }
                        break;
                    }
                    this.sim.beginDecision();
                }
            }

            // Camera follow on the *interpolated* primary player position.
            const p = this.sim.player;
            this.cam.follow(p.x + p.w / 2, VIEW_H / 2);

            // Decay beam visuals.
            if (this.trainingBeams.length) {
                for (const b of this.trainingBeams) b.ttl -= dt;
                this.trainingBeams = this.trainingBeams.filter((b) => b.ttl > 0);
            }
        },

        // Diff prev/current sim state at decision boundaries to fire audio.
        // Fast mode mutes everything to avoid 8× spam.
        diffSimAudio() {
            if (this.fast) { this.prevPlayer = null; return; }
            const p = this.sim.player;
            const prev = this.prevPlayer;
            if (prev) {
                if (!prev.onGround && p.onGround) Audio.land();
                else if (prev.onGround && !p.onGround && p.vy < -200) Audio.jump();

                const stomps = this.sim.stompers;
                const prevAlive = this.prevStomperAlive;
                const n = Math.min(stomps.length, prevAlive ? prevAlive.length : 0);
                for (let i = 0; i < n; i++) {
                    if (prevAlive[i] && !stomps[i].alive) { Audio.stomp(); break; }
                }

                if (this.flyerCooldown <= 0) {
                    for (const f of this.sim.flyers) {
                        if (!f.alive) continue;
                        const dx = (f.x + f.w / 2) - (p.x + p.w / 2);
                        const dy = (f.y + f.h / 2) - (p.y + p.h / 2);
                        if (dx * dx + dy * dy < 80 * 80) {
                            Audio.flyer();
                            this.flyerCooldown = 350;
                            break;
                        }
                    }
                }
            }
            this.prevPlayer = { x: p.x, y: p.y, vx: p.vx, vy: p.vy, onGround: p.onGround };
            this.prevStomperAlive = this.sim.stompers.map((s) => s.alive);
        },

        draw() {
            if (!this.lvlTilemap) return;
            this.lvlTilemap.draw(ctx, this.cam.x, this.cam.y, VIEW_W, VIEW_H);
            if (this.lvlFlag) {
                const f = this.lvlFlag;
                Art.drawFlag(ctx, f.x - this.cam.x, f.y - this.cam.y);
            }
            if (this.lvlPickup && this.sim && !this.sim.pickupCollected) {
                const pk = this.lvlPickup;
                Art.drawPickup(ctx,
                    pk.x - this.cam.x, pk.y - this.cam.y,
                    this.pickupAnimT);
            }
            if (!this.primary) {
                this.drawLoading();
                this.drawHud();
                return;
            }

            // Stompers / flyers come from the sim (mutated as primary plays).
            for (const s of this.sim.stompers) {
                if (!this.cam.visible(s.x, s.y, s.w, s.h)) continue;
                const fr = !s.alive ? 2 : (Math.floor((s.animT || 0) / 200) % 2);
                Art.drawStomper(ctx,
                    s.x - this.cam.x, s.y - this.cam.y, fr);
            }
            for (const fl of this.sim.flyers) {
                if (!fl.alive) continue;
                if (!this.cam.visible(fl.x, fl.y, fl.w, fl.h)) continue;
                const fr = (Math.floor((fl.animT || 0) / 150) % 2);
                Art.drawFlyer(ctx,
                    fl.x - this.cam.x, fl.y - this.cam.y, fr, (fl.vx || 0) > 0);
            }

            // Beam visuals fired by the sim's auto-fire pass. Yellow outer +
            // white-hot core, ttl-faded — same look as play mode.
            for (const b of this.trainingBeams) {
                const a = Math.max(0, b.ttl / b.ttlMax);
                ctx.save();
                ctx.lineCap = 'round';
                ctx.strokeStyle = 'rgba(255, 220, 80, ' + (a * 0.85).toFixed(3) + ')';
                ctx.lineWidth = Game.BEAM_THICKNESS + 6;
                ctx.beginPath();
                ctx.moveTo(b.x0 - this.cam.x, b.y0 - this.cam.y);
                ctx.lineTo(b.x1 - this.cam.x, b.y1 - this.cam.y);
                ctx.stroke();
                ctx.strokeStyle = 'rgba(255, 255, 240, ' + a.toFixed(3) + ')';
                ctx.lineWidth = Math.max(1, Game.BEAM_THICKNESS - 4);
                ctx.beginPath();
                ctx.moveTo(b.x0 - this.cam.x, b.y0 - this.cam.y);
                ctx.lineTo(b.x1 - this.cam.x, b.y1 - this.cam.y);
                ctx.stroke();
                ctx.restore();
            }

            // Ghost player sprites at low alpha. Their tracks are zero-based
            // tick numbers; primary's relative tick = sim.tick - startTick.
            const curTick = this.sim.tick - this.primaryStartTick;
            ctx.save();
            ctx.globalAlpha = 0.30;
            for (const g of this.ghostTracks) {
                const f = lookupGhostFrame(g.frames, curTick);
                if (!f) continue;
                Art.drawHero(ctx,
                    f.x - this.cam.x, f.y - this.cam.y - 2,
                    f.frame, f.facing < 0);
            }
            ctx.restore();

            // Primary hero on top.
            const p = this.sim.player;
            Art.drawHero(ctx,
                p.x - this.cam.x, p.y - this.cam.y - 2,
                heroFrame(p, this.sim.tick), p.facing < 0);

            this.drawHud();
        },

        drawLoading() {
            // Shown until the first trajectory arrives in the ring. During
            // BC warmup + the 5000-step pretrain the trainer is too busy
            // to publish weights, the mcts workers stall on no-weights, and
            // no trajectories complete — so this overlay tells the user
            // something is happening. Once a worker finishes its first
            // episode, primary playback starts and the overlay drops.
            const dots = '.'.repeat(1 + ((Date.now() / 400) | 0) % 3);
            const title = this.warmupInfo
                ? 'Waiting for first run' + dots
                : 'Pretraining the agent' + dots;
            const sub = this.warmupInfo
                ? 'Workers warming up after weights publish'
                : 'Behavior cloning + 5000-step pretrain in progress';
            ctx.save();
            ctx.fillStyle = 'rgba(0,0,0,0.65)';
            const boxW = 520, boxH = 110;
            const bx = Math.floor((VIEW_W - boxW) / 2);
            const by = Math.floor((VIEW_H - boxH) / 2);
            ctx.fillRect(bx, by, boxW, boxH);
            ctx.strokeStyle = 'rgba(255,255,255,0.25)';
            ctx.lineWidth = 1;
            ctx.strokeRect(bx + 0.5, by + 0.5, boxW - 1, boxH - 1);
            ctx.fillStyle = '#fff';
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.font = 'bold 22px monospace';
            ctx.fillText(title, VIEW_W / 2, by + 36);
            ctx.font = '14px monospace';
            ctx.fillStyle = '#cde';
            ctx.fillText(sub + '…', VIEW_W / 2, by + 72);
            ctx.restore();
        },

        drawHud() {
            const w = this.workerStats;
            const pr = this.primary;
            // All workers mirror the same shared tape; pick any worker's
            // reported size as the aggregate.
            let tapeSize = 0, tapeCapacity = 0;
            for (const ms of this.mctsStats) {
                if (ms && ms.tapeSize != null) {
                    tapeSize = ms.tapeSize;
                    tapeCapacity = ms.tapeCapacity || 0;
                    break;
                }
            }
            const lines = [
                'TRAINING — F = fast' + (this.fast ? ' [ON]' : '')
                    + '   C = clear tape   Esc = quit',
                'replay: ep ' + this.episodesPlayed
                    + '   ring ' + this.trajRing.length + '/' + this.TRAJ_RING_SIZE
                    + '   ghosts ' + this.ghostTracks.length
                    + (pr
                        ? ('   primary: ' + pr.reason + ' R=' + pr.totalReturn.toFixed(2)
                           + ' bestX=' + (pr.bestX | 0))
                        : '   [waiting for first trajectory]'),
                'history tape: ' + tapeSize + '/' + tapeCapacity + ' (shared across workers)',
                'pool: ' + this.pool.size + '/' + this.poolCapacity
                    + '   top return ' + this.poolTopReturn.toFixed(2)
                    + '   accepted ' + this.poolAccepted,
            ];
            for (let i = 0; i < this.mctsWorkers.length; i++) {
                const ms = this.mctsStats[i] || {};
                lines.push('mcts#' + (i + 1) + ' (it=' + (this.MCTS_DEPTHS[i] | 0) + '):'
                    + '   ep ' + (ms.episodes | 0)
                    + '   last: ' + (ms.lastReason || 'fresh'));
            }
            if (!this.warmupInfo) {
                const dots = '.'.repeat(1 + ((Date.now() / 400) | 0) % 3);
                lines.push('trainer: warming up (BC + pretrain)' + dots
                    + '   tuples dropped during warmup: ' + (this.droppedTuples | 0));
            } else {
                lines.push('trainer: ingested ' + (w.ingested || 0)
                    + '   buf ' + (w.bufSize || 0)
                    + '   train ' + (w.trainSteps || 0)
                    + '   net v' + (w.netVersion ? w.netVersion.toString() : '0'));
            }
            lines.push('loss  v=' + (+(w.lossValue) || 0).toFixed(4)
                + '   p=' + (+(w.lossPolicy) || 0).toFixed(4)
                + '   mean(20)=' + (+(w.meanReturn) || 0).toFixed(3)
                + '   best=' + (+(w.bestMean) || 0).toFixed(3)
                + (w.resumed ? '   [resumed]' : ''));
            if (this.warmupInfo) {
                const wu = this.warmupInfo;
                if (wu.resumed) {
                    lines.push('warmup: resumed @ mean '
                        + (+wu.meanReturn || 0).toFixed(3));
                } else {
                    lines.push('warmup: kept ' + (wu.kept | 0) + '/' + (wu.attempts | 0)
                        + ' (flag ' + (wu.flags | 0) + ')'
                        + '   tuples ' + (wu.tuplesPushed | 0)
                        + '   pretrain ' + (wu.pretrainSteps | 0)
                        + ' p=' + (+wu.pretrainLossPolicy || 0).toFixed(3));
                }
            }
            ctx.save();
            ctx.fillStyle = 'rgba(0,0,0,0.55)';
            ctx.fillRect(8, 8, 460, 14 * lines.length + 10);
            ctx.fillStyle = '#fff';
            ctx.font = '12px monospace';
            ctx.textBaseline = 'top';
            for (let i = 0; i < lines.length; i++) {
                ctx.fillText(lines[i], 14, 12 + i * 14);
            }
            ctx.restore();
        },

        keydown(key) {
            if (key === 'Escape' || key === 'Esc' || key === 'q' || key === 'Q') {
                this.stop();
                S.switchTo('title');
                return;
            }
            if (key === 'f' || key === 'F') this.fast = !this.fast;
            if (key === 'c' || key === 'C') {
                for (const w of this.mctsWorkers || []) {
                    try { w.postMessage({ type: 'clear_tape' }); } catch (_) {}
                }
            }
        },
    };

    // Hero animation frame from sim player state. `tick` cycles the run
    // animation (one swap every 8 ticks ≈ every 133 ms at 60 Hz).
    function heroFrame(p, tick) {
        if (!p.onGround) return 3;
        if (Math.abs(p.vx) > 8) return 1 + (((tick / 8) | 0) % 2);
        return 0;
    }

    // Binary search for the ghost frame at-or-just-before `tick`. Returns
    // null if the ghost's track ends before `tick` so it vanishes at its
    // death point instead of freezing visible.
    function lookupGhostFrame(frames, tick) {
        if (frames.length === 0 || tick < 0) return null;
        const last = frames[frames.length - 1];
        if (tick > last.tick) return null;
        if (tick <= frames[0].tick) return frames[0];
        let lo = 0, hi = frames.length - 1;
        while (hi - lo > 1) {
            const mid = (lo + hi) >> 1;
            if (frames[mid].tick <= tick) lo = mid; else hi = mid;
        }
        return frames[lo];
    }

    // ── Screens ──────────────────────────────────────────────────────────────
    const S = Screens.create({
        overlay: '#overlay',
        onMenuMove:   Audio.menu,
        onMenuSelect: Audio.select,
        hudSelector:  '#hud',
        hudFor:       ['playing'],
    });

    function pickAction(action) {
        if (action === 'play' || action === 'restart') {
            Game.startRun(); S.switchTo('playing');
        } else if (action === 'train') {
            S.switchTo('training');
        } else if (action === 'demo') {
            S.switchTo('demo');
        } else if (action === 'resume') {
            S.switchTo('playing');
        } else if (action === 'howtoplay') {
            S.switchTo('howtoplay');
        } else if (action === 'back') {
            S.switchTo('title');
        } else if (action === 'quit') {
            S.switchTo('title');
        }
    }

    function defineMenu(name, screenId) {
        S.define(name, {
            enter() { S.showOverlay(screenId || name); S.updateSelection(screenId || name); },
            keydown(key) {
                S.menuNav(screenId || name, key, (idx, item) => {
                    pickAction(item && item.dataset && item.dataset.action);
                });
            },
        });
    }

    defineMenu('title');
    defineMenu('howtoplay');
    defineMenu('pause');
    defineMenu('gameover');
    defineMenu('win');

    S.define('playing', {
        enter() { S.hideOverlay(); },
    });

    S.define('training', {
        enter() {
            S.hideOverlay();
            Training.start();
        },
        keydown(key) { Training.keydown(key); },
    });

    // ── AI Demo ──────────────────────────────────────────────────────────────
    // Loads ckpt/best.bin, drives sim with greedy argmax until pickup, then
    // hands off to a scripted controller that backtracks to spawn and runs
    // back to the flag. SwDemo lives in demo.js.
    const demoCtl = SwDemo.create({
        ctx, Art, Camera2D, Game,
    });
    S.define('demo', {
        enter() {
            S.hideOverlay();
            demoCtl.start();
        },
        keydown(key) {
            if (key === 'Escape' || key === 'Esc' || key === 'q' || key === 'Q') {
                demoCtl.stop();
                S.switchTo('title');
            }
        },
    });

    window.addEventListener('keydown', (e) => S.keydown(e.key));

    function refreshOverlayStats() {
        const gOver = document.getElementById('gameover-stats');
        if (gOver) gOver.textContent = 'Score: ' + Game.score + '   Best: ' + store.get('best');
        const win = document.getElementById('win-stats');
        if (win) win.textContent = 'Score: ' + Game.score + '   Best: ' + store.get('best');
    }
    const _origSwitch = S.switchTo;
    S.switchTo = function (name) {
        if (name === 'gameover' || name === 'win') refreshOverlayStats();
        return _origSwitch.call(S, name);
    };

    // ── Boot ─────────────────────────────────────────────────────────────────
    const loop = GameLoop.create({ tick: update, draw: draw });
    S.switchTo('title');
    loop.start();

    // Expose for debugging in headless / devtools.
    window.__SW = { Game, S, Art, Training };
})();
