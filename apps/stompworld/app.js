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
    SFX.init();
    const Audio = {
        jump:    () => SFX.tone(520, 0.10, 'square',   0.3),
        stomp:   () => SFX.tone(180, 0.08, 'square',   0.5),
        die:     () => SFX.sequence([[300,0.15,'sawtooth',0.5],[200,0.25,'sawtooth',0.5]]),
        win:     () => SFX.sequence([[523,0.10,'square',0.6],[659,0.10,'square',0.6],[784,0.20,'square',0.7]]),
        menu:    () => SFX.tone(400, 0.04, 'sine',     0.3),
        select:  () => SFX.tone(600, 0.08, 'square',   0.4),
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
    const Game = {
        tilemap: null,
        cam: null,
        player: null,
        stompers: [],
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
                    jumpVel:    -680,
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
        if (S.name() === 'training' && Training.sim) {
            Training.draw();
        } else if (Game.tilemap) {
            Game.tilemap.draw(ctx, Game.cam.x, Game.cam.y, VIEW_W, VIEW_H);
            drawFlag();
            drawStompers();
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
                    S.switchTo('gameover');
                } else {
                    Game.respawnPlayer();
                    Game.deathTimer = 0;
                }
            }
            return;
        }

        // Time tick.
        Game.timeLeft -= dt / 1000;
        if (Game.timeLeft <= 0) { killPlayer(); updateHud(); return; }
        if (Math.floor(Game.timeLeft) !== Math.floor(Game.timeLeft + dt / 1000)) {
            updateHud();
        }

        // Player physics.
        Platformer.step(Game.player, {
            left:        Input.down('left'),
            right:       Input.down('right'),
            jumpHeld:    Input.down('primary'),
            jumpPressed: Input.pressed('primary'),
        }, Game.tilemap, dt);

        if (Input.pressed('pause')) { S.switchTo('pause'); return; }

        for (const s of Game.stompers) stepStomper(s, dt);
        handleStompers();
        checkWinLose();

        Game.cam.follow(Game.player.x + Game.player.w / 2, VIEW_H / 2);
    }

    // ── Training mode ────────────────────────────────────────────────────────
    // The displayed game is driven by LiveAgent (low-iter MCTS over the
    // *current* PolicyValueNet snapshot). A background Worker runs the full
    // ExIt training loop and posts fresh weights after every episode it
    // completes, which the live agent loads in place. Watch the same hero
    // get less terrible in real time as the worker churns.
    //
    // Controls: F = run live sim faster (still uses the latest weights),
    //           Esc = back to title.
    const Training = {
        sim: null,
        liveAgent: null,
        worker: null,
        ghosts: null,
        cam: null,
        fast: false,
        FAST_MULT: 8,
        running: false,
        decisionAccum: 0,
        DECISION_PERIOD_MS: 1000 / 60 * 4,   // ≈ FRAME_SKIP * FIXED_DT_MS
        // Stats merged from worker postMessages.
        workerStats: {
            episode: 0, iters: 0, steps: 0, bestX: 0, lastReason: 'fresh',
            lossValue: 0, lossPolicy: 0, trainSteps: 0,
            netVersion: 0n, bufSize: 0,
        },
        liveStats: { episodes: 0, lastReason: 'fresh', bestX: 0, decisions: 0 },

        start() {
            const lvl = Level.load({ tileSize: TILE });
            let spawn = { x: 0, y: 0 };
            const stomperTemplates = [];
            let flag = null;
            for (const e of lvl.entities) {
                if (e.kind === 'player') {
                    spawn.x = e.x; spawn.y = e.y;
                } else if (e.kind === 'stomper') {
                    stomperTemplates.push({
                        x: e.x + 2,
                        y: (e.row + 1) * TILE - 24,
                        w: 28, h: 24, vx: -50, vy: 0,
                        onGround: false, alive: true, squashTimer: 0, animT: 0,
                    });
                } else if (e.kind === 'flag') {
                    flag = { x: e.x, w: 32, h: 96, y: e.row * TILE - 64 };
                    flag.y = e.row * TILE - flag.h + TILE;
                }
            }
            this.sim = SwSim.create({
                tilemap: lvl.tilemap,
                spawn, stompers: stomperTemplates, flag,
                timeLimit: 300,
            });
            this.liveAgent = LiveAgent.create({ sim: this.sim, iterations: 24 });
            this.ghosts = Ghosts.create({ maxGhosts: 6 });
            this.cam = Camera2D.create({
                viewW: VIEW_W, viewH: VIEW_H,
                levelW: lvl.tilemap.widthPx,
                levelH: lvl.tilemap.heightPx,
                deadzoneW: 120, deadzoneH: 1024,
            });
            this.cam.snapTo(this.sim.player.x + this.sim.player.w / 2, VIEW_H / 2);
            this.fast = false;
            this.decisionAccum = 0;
            this.liveStats = { episodes: 0, lastReason: 'fresh', bestX: 0, decisions: 0 };
            this.ghostRec = { frames: [] };

            // Spawn the trainer worker.
            this.worker = new Worker('trainer_worker.js');
            this.worker.onmessage = (e) => {
                const msg = e && e.data; if (!msg) return;
                if (msg.type === 'weights') {
                    this.liveAgent.setWeights(msg.bytes, msg.version);
                    if (msg.stats) Object.assign(this.workerStats, msg.stats);
                } else if (msg.type === 'stats') {
                    if (msg.stats) Object.assign(this.workerStats, msg.stats);
                }
            };
            this.running = true;
        },

        stop() {
            this.running = false;
            if (this.worker) {
                try { this.worker.postMessage({ type: 'stop' }); } catch (_) {}
                try { this.worker.terminate(); } catch (_) {}
                this.worker = null;
            }
        },

        oneDecision() {
            if (!this.liveAgent.weightsLoaded) return;   // wait for first publish
            const action = this.liveAgent.decide();
            const out = this.sim.step(action);
            if (this.sim.player.x > this.liveStats.bestX)
                this.liveStats.bestX = this.sim.player.x;
            this.liveStats.decisions++;

            // Ghost frame.
            const p = this.sim.player;
            let frame = 0;
            if (!p.onGround) frame = 3;
            else if (Math.abs(p.vx) > 8) frame = 1 + (((this.sim.tick / 8) | 0) % 2);
            this.ghostRec.frames.push({
                tick: this.sim.tick, x: p.x, y: p.y, frame, facing: p.facing,
            });

            if (out.done) {
                const reason = this.sim.won ? 'flag'
                             : (this.sim.timeLeft <= 0 ? 'timeout' : 'death');
                this.liveStats.lastReason = reason;
                this.liveStats.episodes++;
                if (this.ghostRec.frames.length > 0) {
                    this.ghosts.commit({ frames: this.ghostRec.frames.slice() });
                    this.ghostRec.frames.length = 0;
                }
                this.sim.reset();
                this.liveStats.bestX = this.sim.player.x;
            }
        },

        update(dt) {
            if (!this.running) return;
            // Pace decisions in real time so the user can watch one hero
            // play. Fast mode just shortens the period; the worker is
            // unaffected (it runs flat-out on its own thread regardless).
            const period = this.fast ? (this.DECISION_PERIOD_MS / this.FAST_MULT)
                                     : this.DECISION_PERIOD_MS;
            this.decisionAccum += dt;
            let safety = 64;
            while (this.decisionAccum >= period && safety-- > 0) {
                this.decisionAccum -= period;
                this.oneDecision();
            }
            this.cam.follow(this.sim.player.x + this.sim.player.w / 2, VIEW_H / 2);
        },

        // Helper: draw one ghost sprite using the hero atlas.
        drawGhostSprite(ctx, x, y, frame, flipped, alpha) {
            const drawX = Math.round(x - this.cam.x);
            const drawY = Math.round(y - this.cam.y - 2);
            ctx.globalAlpha = alpha;
            Art.drawHero(ctx, drawX, drawY, frame, flipped);
            ctx.globalAlpha = 1;
        },

        draw() {
            if (!this.sim) return;
            const tm = this.sim.tilemap;
            tm.draw(ctx, this.cam.x, this.cam.y, VIEW_W, VIEW_H);

            // Flag.
            if (this.sim.flag) {
                Art.drawFlag(ctx,
                             Math.round(this.sim.flag.x - this.cam.x),
                             Math.round(this.sim.flag.y - this.cam.y));
            }

            // Stompers (live).
            for (const s of this.sim.stompers) {
                if (!this.cam.visible(s.x, s.y, s.w, s.h)) continue;
                const f = !s.alive ? 2 : (Math.floor(s.animT / 200) % 2);
                Art.drawStomper(ctx,
                                Math.round(s.x - this.cam.x),
                                Math.round(s.y - this.cam.y), f);
            }

            // Ghosts (under live hero).
            const t = this.sim.tick;
            this.ghosts.draw(ctx, t, (cctx, x, y, fr, flipped, a) =>
                this.drawGhostSprite(cctx, x, y, fr, flipped, a));

            // Live hero.
            const p = this.sim.player;
            let frame = 0;
            if (!p.onGround) frame = 3;
            else if (Math.abs(p.vx) > 8) frame = 1 + (((this.sim.tick / 8) | 0) % 2);
            Art.drawHero(ctx,
                         Math.round(p.x - this.cam.x),
                         Math.round(p.y - this.cam.y - 2),
                         frame, p.facing < 0);

            // HUD overlay (drawn in the virtual viewport so it letterboxes).
            this.drawHud();
        },

        drawHud() {
            const w = this.workerStats;
            const l = this.liveStats;
            const ready = this.liveAgent && this.liveAgent.weightsLoaded;
            const lines = [
                'TRAINING — F = fast' + (this.fast ? ' [ON]' : '') + '   Esc = quit',
                'live: ep ' + l.episodes + '   bestX ' + (l.bestX | 0)
                    + '   last: ' + l.lastReason
                    + (ready ? '' : '   [waiting for weights]'),
                'worker: ep ' + w.episode + '   iters ' + w.iters + '   steps ' + w.steps,
                'buf ' + (w.bufSize || 0) + '   train_steps ' + (w.trainSteps || 0)
                    + '   net v' + (w.netVersion ? w.netVersion.toString() : '0'),
                'loss  v=' + (w.lossValue || 0).toFixed(4)
                    + '   p=' + (w.lossPolicy || 0).toFixed(4),
            ];
            ctx.save();
            ctx.fillStyle = 'rgba(0,0,0,0.55)';
            ctx.fillRect(8, 8, 360, 14 * lines.length + 10);
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
