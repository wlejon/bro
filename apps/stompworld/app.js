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
        { name: 'up',      label: 'Menu Up',   defaults: ['ArrowUp'] },
        { name: 'down',    label: 'Menu Down', defaults: ['ArrowDown'] },
        { name: 'confirm', label: 'Confirm',   defaults: ['Enter'] },
        { name: 'pause',   label: 'Pause',     defaults: ['Escape', 'p'] },
    ]);
    Input.attach(window);

    // ── Storage ──────────────────────────────────────────────────────────────
    const store = Storage.create('stompworld');
    store.load({ best: 0 });

    // ── Game state ───────────────────────────────────────────────────────────
    // Flyer template: returns a populated runtime entity from a level entity.
    function makeFlyer(e) {
        const bob = e.kind === 'flyer_bob';
        const cx = e.col * TILE + TILE / 2;
        const cy = e.row * TILE + TILE / 2;
        const FLY_W = 24, FLY_H = 16;
        return {
            x: cx - FLY_W / 2, y: cy - FLY_H / 2,
            w: FLY_W, h: FLY_H,
            vx: -80, vy: 0,
            spawnX: cx - FLY_W / 2,
            spawnY: cy - FLY_H / 2,
            patrolRange: 96,
            bobAmp:  bob ? 32 : 0,
            bobFreq: bob ? Math.PI : 0,    // ~2 s period
            bobT:    0,
            animT:   0,
        };
    }

    const Game = {
        tilemap: null,
        cam: null,
        player: null,
        stompers: [],
        flyers: [],
        flag: null,
        score: 0,
        lives: 3,
        timeLeft: 300,
        runAccum: 0,    // for animating run frames
        deathTimer: 0,  // > 0 = death anim playing
        winTimer:   0,  // > 0 = win anim playing
        spawn: { x: 0, y: 0 },

        loadLevel() {
            const lvl = Level.load({ tileSize: TILE });
            this.tilemap  = lvl.tilemap;
            this.stompers = [];
            this.flyers   = [];
            this.flag = null;

            for (const e of lvl.entities) {
                if (e.kind === 'player') {
                    this.spawn.x = e.x;
                    this.spawn.y = e.y;
                } else if (e.kind === 'stomper') {
                    // Spawn so the stomper's feet sit at the bottom of its tile cell.
                    // The level placement convention: 'G' is on the row directly above ground,
                    // so its bottom-edge aligns with the top of the ground row.
                    this.stompers.push({
                        x: e.x + 2,
                        y: (e.row + 1) * TILE - 24,
                        w: 28, h: 24,
                        vx: -50, vy: 0,
                        onGround: false,
                        alive: true,
                        squashTimer: 0,
                        animT: 0,
                    });
                } else if (e.kind === 'flyer' || e.kind === 'flyer_bob') {
                    this.flyers.push(makeFlyer(e));
                } else if (e.kind === 'flag') {
                    this.flag = { x: e.x, y: e.y * 0 + (e.row * TILE) - 64, h: 96, w: 32 };
                    // Anchor the flag's foot to the ground row immediately below its spawn.
                    this.flag.y = e.row * TILE - this.flag.h + TILE;
                }
            }

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
            // Re-create the mob layout from the level definition so
            // squashed/displaced stompers come back when the player
            // respawns. Keeps the array reference stable in case the
            // renderer holds onto it.
            this.stompers.length = 0;
            this.flyers.length = 0;
            const lvl = Level.load({ tileSize: TILE });
            for (const e of lvl.entities) {
                if (e.kind === 'stomper') {
                    this.stompers.push({
                        x: e.x + 2,
                        y: (e.row + 1) * TILE - 24,
                        w: 28, h: 24,
                        vx: -50, vy: 0,
                        onGround: false,
                        alive: true,
                        squashTimer: 0,
                        animT: 0,
                    });
                } else if (e.kind === 'flyer' || e.kind === 'flyer_bob') {
                    this.flyers.push(makeFlyer(e));
                }
            }
            this.cam.snapTo(this.player.x + this.player.w / 2, VIEW_H / 2);
        },

        startRun() {
            this.score = 0; this.lives = 3; this.timeLeft = 300;
            this.deathTimer = 0; this.winTimer = 0;
            this.loadLevel();
            updateHud();
        },
    };

    // ── HUD ──────────────────────────────────────────────────────────────────
    const hudScore = document.getElementById('hud-score');
    const hudLives = document.getElementById('hud-lives');
    const hudTime  = document.getElementById('hud-time');
    function updateHud() {
        hudScore.textContent = Game.score;
        hudLives.textContent = Game.lives;
        hudTime.textContent  = Math.max(0, Math.ceil(Game.timeLeft));
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
            if (!s.alive) continue;
            if (p.x + p.w <= s.x || p.x >= s.x + s.w) continue;
            if (p.y + p.h <= s.y || p.y >= s.y + s.h) continue;
            // From above (falling and feet near stomper top) = stomp.
            const fromAbove = p.vy > 0 && (p.y + p.h - s.y) < 16;
            if (fromAbove) {
                s.alive = false;
                s.squashTimer = 350;
                p.vy = -380;
                Game.score += 100;
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
                Game.score += 1000;
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
                     Math.round(p.x - Game.cam.x),
                     Math.round(p.y - Game.cam.y - 2),
                     frame, p.facing < 0);
    }

    function drawStompers() {
        for (const s of Game.stompers) {
            if (!Game.cam.visible(s.x, s.y, s.w, s.h)) continue;
            const frame = !s.alive ? 2 : (Math.floor(s.animT / 200) % 2);
            Art.drawStomper(ctx,
                            Math.round(s.x - Game.cam.x),
                            Math.round(s.y - Game.cam.y),
                            frame);
        }
    }

    function drawFlyers() {
        for (const f of Game.flyers) {
            if (!Game.cam.visible(f.x, f.y, f.w, f.h)) continue;
            const frame = (Math.floor(f.animT / 150) % 2);
            Art.drawFlyer(ctx,
                          Math.round(f.x - Game.cam.x),
                          Math.round(f.y - Game.cam.y),
                          frame, f.vx > 0);
        }
    }

    function drawFlag() {
        if (!Game.flag) return;
        const f = Game.flag;
        Art.drawFlag(ctx, Math.round(f.x - Game.cam.x), Math.round(f.y - Game.cam.y));
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
        } else if (Game.tilemap) {
            Game.tilemap.draw(ctx, Game.cam.x, Game.cam.y, VIEW_W, VIEW_H);
            drawFlag();
            drawStompers();
            drawFlyers();
            drawHero();
        }
        ctx.restore();
    }

    // ── Update ───────────────────────────────────────────────────────────────
    function update(dt) {
        if (S.name() === 'training') { Training.update(dt); return; }
        if (S.name() !== 'playing') return;

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

        for (const s of Game.stompers) stepStomper(s, dt);
        for (const f of Game.flyers)   stepFlyer(f, dt);
        handleStompers();
        handleFlyers();
        checkWinLose();

        Game.cam.follow(Game.player.x + Game.player.w / 2, VIEW_H / 2);
    }

    // ── Training mode ────────────────────────────────────────────────────────
    // Layered architecture, all heavy work off the main thread:
    //
    //   trainer_worker      owns net + buffer + SGD; ingests tuples,
    //                       publishes weights, persists checkpoints.
    //   mcts_worker × N     pure self-play data generators at varying
    //                       search depths (Dirichlet exploration on).
    //                       No render, no failure memory.
    //   live_worker         owns the displayed sim. Runs MCTS on top of
    //                       the best-crop pool's seeds, biased away from
    //                       recently-failed lines via FailureTape. Posts
    //                       a render snapshot per decision.
    //
    // Main thread:
    //   - routes: tuples (any worker → trainer), weights (trainer → all
    //     play workers), trajectory_end (any → trainer for ckpt metric).
    //   - holds the BestCrop pool: every trajectory from any worker is
    //     ingested and ranked. Periodically (~SEED_PERIOD_MS) main picks
    //     a top entry and seeds the live worker with either the start
    //     state alone or a replayed prefix of its action sequence — so
    //     the display agent runs MCTS on top of the best raw data we
    //     have, not from scratch.
    //   - draws from the latest render snapshot at 60 fps regardless of
    //     how slow any individual MCTS search runs.
    //
    // Controls: F = bump live decision rate, C = clear failure tape,
    //           Esc = back to title.
    const Training = {
        trainer: null,
        live: null,
        mctsWorkers: [],
        pool: null,
        cam: null,
        snap: null,
        fast: false,
        FAST_MULT: 8,
        running: false,

        // Static level data for rendering — sims live in the workers.
        lvlTilemap: null,
        lvlFlag: null,

        // Worker-config knobs. Two MCTS data workers at very different
        // search depths so the trainer sees a mix of cheap/diverse and
        // deep/confident visit distributions.
        NUM_MCTS_WORKERS: 2,
        MCTS_DEPTHS:  [50, 200],
        MCTS_ROLLOUT: [6, 10],

        // Seed pump: how often main hands the live worker a fresh
        // start-state from the best-crop pool.
        seedAccum: 0,
        SEED_PERIOD_MS: 500,

        // Live decision pump (main → live worker). Both workers self-clock
        // via 'ready' messages and main answers with 'step' (live) or
        // 'tick' (mcts). For live we also enforce wall-clock pacing — if
        // a 'ready' arrives before LIVE_PERIOD_MS has elapsed since the
        // last decision, we hold the credit until the period passes so
        // the displayed agent runs in real time. Fast mode shrinks the
        // period.
        LIVE_PERIOD_MS: 1000 / 60 * 4,    // matches sim FRAME_SKIP
        liveLastStep: 0,
        liveCredits: 0,

        // mcts workers are self-clocking: they post 'ready' after each
        // batch, and main responds with another 'tick'.
        mctsReady: [],

        // Audio diff state — compare last vs current render snap to fire
        // sounds on transitions (stomp, land, jump, death/win episode end).
        // Skipped while fast mode is on so we don't spray at 8x speed.
        prevAudioSnap: null,
        prevEpisodes: 0,
        prevReason: 'fresh',
        flyerCooldown: 0,

        // HUD aggregates.
        workerStats: {
            ingested: 0, bufSize: 0, trainSteps: 0,
            lossValue: 0, lossPolicy: 0,
            netVersion: 0n, bestMean: 0, meanReturn: 0, resumed: 0,
        },
        liveStats: {
            episodes: 0, lastReason: 'fresh', bestX: 0,
            decisions: 0, tapeEntries: 0, tapeSigs: 0,
        },
        mctsStats: [],
        warmupInfo: null,
        poolTopReturn: 0,
        poolCapacity: 32,

        start() {
            const lvl = Level.load({ tileSize: TILE });
            this.lvlTilemap = lvl.tilemap;
            let flag = null;
            for (const e of lvl.entities) {
                if (e.kind === 'flag') {
                    flag = { x: e.x, w: 32, h: 96, y: e.row * TILE - 64 };
                    flag.y = e.row * TILE - flag.h + TILE;
                }
            }
            this.lvlFlag = flag;

            this.cam = Camera2D.create({
                viewW: VIEW_W, viewH: VIEW_H,
                levelW: lvl.tilemap.widthPx,
                levelH: lvl.tilemap.heightPx,
                deadzoneW: 120, deadzoneH: 1024,
            });
            this.cam.snapTo(VIEW_W / 2, VIEW_H / 2);

            this.snap = null;
            this.prevAudioSnap = null;
            this.prevEpisodes = 0;
            this.prevReason = 'fresh';
            this.flyerCooldown = 0;
            this.pool = BestCrop.create({ capacity: this.poolCapacity });
            this.poolTopReturn = 0;
            this.fast = false;
            this.warmupInfo = null;
            this.mctsStats = [];
            this.mctsReady = [];
            this.seedAccum = 0;
            this.liveLastStep = 0;
            this.liveCredits = 0;
            for (let i = 0; i < this.NUM_MCTS_WORKERS; i++) {
                this.mctsStats.push({});
                this.mctsReady.push(false);
            }

            this.trainer = new Worker('trainer_worker.js');
            this.trainer.onmessage = (e) => this.onTrainerMessage(e && e.data);

            this.live = new Worker('live_worker.js');
            this.live.onmessage = (e) => this.onLiveMessage(e && e.data);

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
            const all = [this.trainer, this.live, ...(this.mctsWorkers || [])]
                .filter(Boolean);
            for (const w of all) {
                try { w.postMessage({ type: 'stop' }); } catch (_) {}
                try { w.terminate(); } catch (_) {}
            }
            this.trainer = null;
            this.live = null;
            this.mctsWorkers = [];
        },

        // Each play-worker recipient needs its own ArrayBuffer so we can
        // hand them off zero-copy via the transferList. Cloning the small
        // weights blob N times costs less than the cross-thread copy
        // postMessage would otherwise do for N recipients.
        broadcastWeights(bytes, version) {
            const recipients = [this.live, ...this.mctsWorkers].filter(Boolean);
            for (let i = 0; i < recipients.length; i++) {
                const copy = new Uint8Array(bytes.length);
                copy.set(bytes);
                try {
                    recipients[i].postMessage({
                        type: 'weights', bytes: copy, version,
                    }, [copy.buffer]);
                } catch (_) {}
            }
        },

        onTrainerMessage(m) {
            if (!m) return;
            if (m.type === 'weights') {
                this.broadcastWeights(m.bytes, m.version);
                if (m.stats) Object.assign(this.workerStats, m.stats);
            } else if (m.type === 'stats') {
                if (m.stats) Object.assign(this.workerStats, m.stats);
            } else if (m.type === 'warmup') {
                this.warmupInfo = m.stats || {};
            }
        },

        onLiveMessage(m) {
            if (!m) return;
            if (m.type === 'render') {
                this.snap = m.snap;
                const s = m.snap;
                if (s.episodes    != null) this.liveStats.episodes    = s.episodes;
                if (s.lastReason  != null) this.liveStats.lastReason  = s.lastReason;
                if (s.bestX       != null) this.liveStats.bestX       = s.bestX | 0;
                if (s.decisions   != null) this.liveStats.decisions   = s.decisions;
                if (s.tapeEntries != null) this.liveStats.tapeEntries = s.tapeEntries;
                if (s.tapeSigs    != null) this.liveStats.tapeSigs    = s.tapeSigs;
            } else if (m.type === 'tuples') {
                this.routeTuples(m);
            } else if (m.type === 'trajectory') {
                this.ingestTrajectory(m);
            } else if (m.type === 'ready') {
                // Worker finished a decision (or stalled on no-weights).
                // We grant a 'step' as soon as the LIVE_PERIOD_MS pacing
                // budget allows. If not yet, increment credits and let
                // pumpLive flush them when wall time catches up.
                this.liveCredits++;
                this.tryReleaseLiveCredit();
            }
        },

        tryReleaseLiveCredit() {
            if (!this.live || this.liveCredits <= 0) return;
            const period = this.fast
                ? this.LIVE_PERIOD_MS / this.FAST_MULT
                : this.LIVE_PERIOD_MS;
            const now = Date.now();
            if (now - this.liveLastStep < period) return;
            this.liveLastStep = now;
            this.liveCredits--;
            try { this.live.postMessage({ type: 'step' }); } catch (_) {}
        },

        onMctsMessage(m, idx) {
            if (!m) return;
            if (m.type === 'tuples') {
                this.routeTuples(m);
            } else if (m.type === 'trajectory') {
                this.ingestTrajectory(m);
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
            this.pool.ingest({
                startSnap:   m.startSnap,
                actions:     m.actions,
                decisions:   m.decisions,
                totalReturn: m.totalReturn,
                searchDepth: m.searchDepth,
                reason:      m.reason,
                source:      m.source,
                bestX:       m.bestX,
            });
            this.poolTopReturn = this.pool.topReturn();
            if (this.trainer) {
                try {
                    this.trainer.postMessage({
                        type: 'trajectory_end',
                        totalReturn: m.totalReturn,
                        reason: m.reason,
                    });
                } catch (_) {}
            }
        },

        // The live worker always starts at the level's default spawn
        // (col 2). The pool keeps collecting trajectories from all
        // workers as training data, but we don't use it to teleport
        // the displayed agent — the user wants every visible run to
        // start at the beginning of the map. The "search on top of
        // search" layering still happens implicitly: the trainer pulls
        // tuples from the deep mcts workers and republishes weights,
        // and the live agent's shallow MCTS refines on top of those
        // weights. Just no state-restore funny business.
        pumpSeed(dt) {
            this.seedAccum += dt;
            if (this.seedAccum < this.SEED_PERIOD_MS) return;
            this.seedAccum = 0;
            this.pool.step();   // age entries even if we don't seed from it
        },

        update(dt) {
            if (!this.running) return;
            this.pumpSeed(dt);
            // Try to release any banked live-decision credits now that
            // wall time has advanced. Credits only accumulate when the
            // worker is faster than the period; if MCTS is slower than
            // real time the worker can't keep up and we just don't send.
            this.tryReleaseLiveCredit();
            if (this.snap && this.snap.player) {
                this.cam.follow(
                    this.snap.player.x + this.snap.player.w / 2, VIEW_H / 2);
                this.diffSnapAudio(dt);
            }
        },

        // Snapshot diffing on the main thread is the cheap path: the live
        // worker doesn't know about audio, so we infer events by comparing
        // the most recent two render snaps. Fast mode mutes this entirely
        // so 8× playback doesn't machine-gun the speakers.
        diffSnapAudio(dt) {
            if (this.fast) { this.prevAudioSnap = this.snap; return; }
            const cur = this.snap;
            const prev = this.prevAudioSnap;
            if (this.flyerCooldown > 0) this.flyerCooldown -= dt;

            // Episode boundary — episodes counter ticked since last frame.
            // cur.lastReason is the reason the *previous* episode ended,
            // posted on the spawn frame of the new one.
            if (cur.episodes !== this.prevEpisodes) {
                if (cur.lastReason === 'flag')      Audio.win();
                else if (cur.lastReason === 'death') Audio.die();
                this.prevEpisodes = cur.episodes;
                this.prevAudioSnap = cur;
                return;
            }

            if (prev && prev.player && cur.player) {
                const a = prev.player, b = cur.player;
                if (!a.onGround && b.onGround) Audio.land();
                else if (a.onGround && !b.onGround && b.vy < -200) Audio.jump();

                // Stomper kills: any stomper that flipped alive → dead.
                const ps = prev.stompers || [];
                const cs = cur.stompers || [];
                const n = Math.min(ps.length, cs.length);
                for (let i = 0; i < n; i++) {
                    if (ps[i].alive && !cs[i].alive) { Audio.stomp(); break; }
                }

                // Flyer proximity warning — quiet noise blip when one
                // gets close to the player. Cooldown so it doesn't loop
                // every frame when the agent hugs a flyer.
                if (this.flyerCooldown <= 0 && cur.flyers) {
                    for (const f of cur.flyers) {
                        const dx = (f.x + f.w / 2) - (b.x + b.w / 2);
                        const dy = (f.y + f.h / 2) - (b.y + b.h / 2);
                        if (dx * dx + dy * dy < 80 * 80) {
                            Audio.flyer();
                            this.flyerCooldown = 350;
                            break;
                        }
                    }
                }
            }
            this.prevAudioSnap = cur;
        },

        draw() {
            if (!this.lvlTilemap) return;
            this.lvlTilemap.draw(ctx, this.cam.x, this.cam.y, VIEW_W, VIEW_H);
            if (this.lvlFlag) {
                const f = this.lvlFlag;
                Art.drawFlag(ctx,
                    Math.round(f.x - this.cam.x),
                    Math.round(f.y - this.cam.y));
            }
            if (this.snap) {
                for (const s of this.snap.stompers) {
                    if (!this.cam.visible(s.x, s.y, s.w, s.h)) continue;
                    const fr = !s.alive ? 2 : (Math.floor(s.animT / 200) % 2);
                    Art.drawStomper(ctx,
                        Math.round(s.x - this.cam.x),
                        Math.round(s.y - this.cam.y), fr);
                }
                for (const fl of this.snap.flyers) {
                    if (!this.cam.visible(fl.x, fl.y, fl.w, fl.h)) continue;
                    const fr = (Math.floor(fl.animT / 150) % 2);
                    Art.drawFlyer(ctx,
                        Math.round(fl.x - this.cam.x),
                        Math.round(fl.y - this.cam.y), fr, fl.vx > 0);
                }
                const p = this.snap.player;
                let frame = 0;
                if (!p.onGround) frame = 3;
                else if (Math.abs(p.vx) > 8) frame = 1 + (((this.snap.tick / 8) | 0) % 2);
                Art.drawHero(ctx,
                    Math.round(p.x - this.cam.x),
                    Math.round(p.y - this.cam.y - 2),
                    frame, p.facing < 0);
            }
            this.drawHud();
        },

        drawHud() {
            const w = this.workerStats;
            const l = this.liveStats;
            const lines = [
                'TRAINING — F = fast' + (this.fast ? ' [ON]' : '')
                    + '   C = clear failures   Esc = quit',
                'live: ep ' + l.episodes + '   bestX ' + (l.bestX | 0)
                    + '   last: ' + l.lastReason
                    + '   decisions ' + l.decisions
                    + (this.snap ? '' : '   [waiting for first snap]'),
                'failure tape: ' + l.tapeSigs + ' sigs / ' + l.tapeEntries + ' entries',
                'pool: ' + this.pool.size() + '/' + this.poolCapacity
                    + '   top return ' + this.poolTopReturn.toFixed(2)
                    + '   accepted ' + this.pool.totalAccepted(),
            ];
            for (let i = 0; i < this.mctsWorkers.length; i++) {
                const ms = this.mctsStats[i] || {};
                lines.push('mcts#' + (i + 1) + ' (it=' + (this.MCTS_DEPTHS[i] | 0) + '):'
                    + '   ep ' + (ms.episodes | 0)
                    + '   last: ' + (ms.lastReason || 'fresh'));
            }
            lines.push('trainer: ingested ' + (w.ingested || 0)
                + '   buf ' + (w.bufSize || 0)
                + '   train ' + (w.trainSteps || 0)
                + '   net v' + (w.netVersion ? w.netVersion.toString() : '0'));
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
            if (key === 'f' || key === 'F') {
                this.fast = !this.fast;
                // Pacing happens in main's pumpLive via FAST_MULT;
                // worker doesn't need to know.
            }
            if (key === 'c' || key === 'C') {
                if (this.live) {
                    try { this.live.postMessage({ type: 'clear_failures' }); } catch (_) {}
                }
            }
        },
    };

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
