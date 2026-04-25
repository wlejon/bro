// app.js — Pegbounce entry point.
//
// Physics approach: Jolt (engine-bound), driven through physics.js's wrapper.
// Each level instantiates its own sandbox world handle (Physics.createWorld
// builds a Physics.createWorldHandle internally), so we get isolation between
// the live shot and the Mirage trajectory predict.
//
// Slow-mo (Option C from the migration plan): physics.js scales the dt it
// passes to handle.step() while world.slowmo > 0. That keeps every visual
// effect — particle motion, ball trail, peg-light timing — smoothly slowed
// without poking the engine's auto-stepped default world.
//
// High-level structure:
//   • state — UI mode, current level/world, score/combo accumulators
//   • main loop — tick drives physics, draw paints the field and HUD
//   • screens — apps/lib/screens.js overlay state machine (title, levels,
//               guide select, pause, clear, fail, settings, credits)
//   • test hooks — window.__pegbounce exposes deterministic helpers for
//                  the headless test harness (apps/pegbounce/test.js).

'use strict';
(function () {
    const canvas = document.getElementById('game');
    const ctx    = canvas.getContext('2d');
    const FIELD_W = Physics.FIELD_W, FIELD_H = Physics.FIELD_H;

    function canvasSize() {
        return Canvas.size(ctx, FIELD_W, FIELD_H);
    }

    // ----------------- Persistent settings / progress -----------------------
    const store = Storage.create('pegbounce');
    store.load({
        unlocked:    1,          // highest unlocked level index + 1
        best:        {},         // levelId -> best score
        stars:       {},         // levelId -> star count (1..3)
        selectedGuide: 'wingtip',
        sfxVol:      0.8,
        musicVol:    0.6,
        trajectory:  true,
        screenshake: true,
    });

    // ----------------- Input -----------------------------------------------
    Input.init([
        { name: 'aim_left',  label: 'Aim Left',  defaults: ['a', 'ArrowLeft']  },
        { name: 'aim_right', label: 'Aim Right', defaults: ['d', 'ArrowRight'] },
        { name: 'launch',    label: 'Launch',    defaults: [' ', 'Enter']      },
        { name: 'pause',     label: 'Pause',     defaults: ['Escape']          },
        { name: 'up',        label: 'Menu Up',   defaults: ['ArrowUp']         },
        { name: 'down',      label: 'Menu Down', defaults: ['ArrowDown']       },
        { name: 'confirm',   label: 'Confirm',   defaults: ['Enter']           },
        { name: 'back',      label: 'Back',      defaults: ['Backspace']       },
    ], { storageKey: 'pegbounce:controls' });
    Input.attach(window);

    // ----------------- Audio ------------------------------------------------
    Sfx.init(store.get('sfxVol'));

    // ----------------- Screens manager -------------------------------------
    const screens = Screens.create({
        overlay:      '#overlay',
        onMenuMove:   Sfx.menuMove,
        onMenuSelect: Sfx.menuSelect,
    });

    // ----------------- Game state ------------------------------------------
    const S = {
        world: null,
        levelIdx: 0,
        balls: 10,                 // balls remaining to shoot
        ballsStart: 10,
        score: 0,
        mult: 1,
        shotScore: 0,
        shotOrangeCount: 0,
        comboCount: 0,             // pegs hit this shot
        totalOrangeStart: 0,
        levelClearTriggered: false,
        levelFailTriggered: false,
        shotInProgress: false,
        aimAngle: Math.PI / 2,     // straight down
        mouseOverCanvas: false,
        cannonX: FIELD_W / 2,
        cannonY: 40,
        particles: Particles.createSystem(),
        fx: { shake: 0 },
        purpleActive: false,
        guideId: store.get('selectedGuide') || 'wingtip',
        bonusBallsAwarded: 0,
        shotSeed: 1,
        mirageShowing: false,
        mirageTimer: 0,
        trajectoryPoints: [],      // mirage guide or aim-preview
        lastLaunchSpeed: 820,
        screenshotHintTimer: 0,
        // replay buffer for deterministic test hook
        replayLog: [],
    };

    // ----------------- Level loading ---------------------------------------
    function loadLevel(idx, opts) {
        opts = opts || {};
        S.levelIdx = idx;
        const lv = Levels.LEVELS[idx];
        // Tear down the previous world's Jolt sandbox handle before building a new one.
        if (S.world) Physics.destroyWorld(S.world);
        S.world = Levels.buildLevel(idx, opts.seed != null ? opts.seed : (idx * 73 + 11));
        S.balls = lv.balls;
        S.ballsStart = lv.balls;
        S.score = 0;
        S.mult = 1;
        S.shotScore = 0;
        S.shotOrangeCount = 0;
        S.comboCount = 0;
        S.totalOrangeStart = Physics.countRemainingOrange(S.world);
        S.levelClearTriggered = false;
        S.levelFailTriggered  = false;
        S.shotInProgress      = false;
        S.aimAngle            = Math.PI / 2;
        S.purpleActive        = false;
        S.bonusBallsAwarded   = 0;
        S.mirageShowing       = false;
        S.fx.shake            = 0;
        S.trajectoryPoints    = [];
        Particles.clear(S.particles);
    }

    // ----------------- Aim helpers -----------------------------------------
    function updateAimFromMouse(mx, my) {
        const rect = canvas.getBoundingClientRect();
        const dims = canvasSize();
        const sx = (mx - rect.left) * (dims.w / rect.width);
        const sy = (my - rect.top)  * (dims.h / rect.height);
        // Map screen -> physics field coords (letterboxed fit).
        const fit = fitScale(dims.w, dims.h);
        const fx = (sx - fit.offX) / fit.scale;
        const fy = (sy - fit.offY) / fit.scale;
        let ang = Math.atan2(fy - S.cannonY, fx - S.cannonX);
        // Clamp to downward arc so shooters can't aim up.
        ang = Math.max(0.08, Math.min(Math.PI - 0.08, ang));
        S.aimAngle = ang;
    }

    function fitScale(cw, ch) {
        const s = Math.min(cw / FIELD_W, ch / FIELD_H);
        const w = FIELD_W * s, h = FIELD_H * s;
        return { scale: s, offX: (cw - w) * 0.5, offY: (ch - h) * 0.5 };
    }

    canvas.addEventListener('mousemove', (e) => {
        S.mouseOverCanvas = true;
        updateAimFromMouse(e.clientX, e.clientY);
    });
    canvas.addEventListener('mouseleave', () => { S.mouseOverCanvas = false; });
    canvas.addEventListener('click', (e) => {
        if (screens.name() !== 'playing') return;
        updateAimFromMouse(e.clientX, e.clientY);
        tryLaunch();
    });

    // ----------------- Shot lifecycle --------------------------------------
    function tryLaunch() {
        if (S.shotInProgress) return;
        if (S.balls <= 0) return;
        if (S.levelClearTriggered || S.levelFailTriggered) return;
        const speed = S.lastLaunchSpeed;
        S.shotInProgress      = true;
        S.shotScore           = 0;
        S.shotOrangeCount     = 0;
        S.comboCount          = 0;
        S.mult                = 1;
        S.purpleActive        = false;
        S.balls -= 1;
        // Launch from the muzzle tip — same offset the Mirage predict and the
        // short aim arc use so the previewed trajectory matches the live one.
        const muzzleX = S.cannonX + Math.cos(S.aimAngle) * 30;
        const muzzleY = S.cannonY + Math.sin(S.aimAngle) * 30;
        Physics.launchBall(S.world, S.aimAngle, speed, muzzleX, muzzleY);
        S.replayLog.push({ kind: 'launch', a: S.aimAngle, s: speed });
        Sfx.launch();
    }

    // When the shot ends, sweep lit pegs, update totals, check clear/fail.
    function finishShot() {
        const removed = Physics.sweepLit(S.world);
        // Particle burst for each removed peg.
        for (const p of removed) {
            const col = pegColor(p.type);
            Particles.burst(S.particles, p.x, p.y, col, 12, 220);
        }
        // Catch bar save?
        if (S.world.caughtThisShot) {
            S.balls += 1;
            Hud.toast('Free ball!', 1000, {
                id: 'pb-toast',
                container: document.body,
            });
            Sfx.catchGet();
            S.world.caughtThisShot = false;
        }

        S.score += S.shotScore;
        const remOr = Physics.countRemainingOrange(S.world);
        const cleared = S.totalOrangeStart - remOr;
        // Bonus balls: 10 cleared (cumulative) => 1 extra.
        const targetBonus = Math.floor(cleared / 10);
        while (S.bonusBallsAwarded < targetBonus) {
            S.balls += 1;
            S.bonusBallsAwarded++;
            Hud.toast('Bonus ball!', 1000, { id: 'pb-toast', container: document.body });
        }

        S.shotInProgress = false;

        if (remOr === 0 && !S.levelClearTriggered) {
            S.levelClearTriggered = true;
            setTimeout(onLevelClear, 400);
            return;
        }
        if (S.balls <= 0 && !S.levelFailTriggered) {
            S.levelFailTriggered = true;
            setTimeout(onLevelFail, 400);
            return;
        }
    }

    function onLevelClear() {
        const lv = Levels.LEVELS[S.levelIdx];
        const bestMap  = store.get('best')  || {};
        const starMap  = store.get('stars') || {};
        const prevBest = bestMap[lv.id] || 0;
        if (S.score > prevBest) { bestMap[lv.id] = S.score; }
        const stars = starCount(S.score, lv.stars);
        if (stars > (starMap[lv.id] || 0)) starMap[lv.id] = stars;
        const unlocked = Math.max(store.get('unlocked') || 1, S.levelIdx + 2);
        store.set('best', bestMap);
        store.set('stars', starMap);
        store.set('unlocked', Math.min(unlocked, Levels.LEVELS.length));
        store.save();
        Sfx.levelClear();
        screens.switchTo('clear');
    }

    function onLevelFail() {
        Sfx.levelFail();
        screens.switchTo('fail');
    }

    function starCount(score, thresholds) {
        let n = 0;
        for (const t of thresholds) if (score >= t) n++;
        return n;
    }

    // ----------------- Scoring / event draining ----------------------------
    // Called after each physics step. Converts queued events into score,
    // combo, guide triggers, etc.
    function drainEvents() {
        const ev = S.world.scoreEvents;
        if (!ev.length) return;
        Physics.markLitFromEvents(S.world, ev);
        for (const e of ev) {
            if (e.kind === 'peg-hit') {
                const peg = e.peg;
                // Only award on *first* hit per shot (lit-just-now).
                if (!peg._scoredShot || peg._scoredShot !== S.world.shotIndex) {
                    peg._scoredShot = S.world.shotIndex;
                    S.comboCount++;
                    let pts = 10;
                    if (peg.type === Physics.PEG.ORANGE) {
                        pts = 100;
                        S.shotOrangeCount++;
                        Sfx.orangeHit(S.comboCount);
                    } else if (peg.type === Physics.PEG.PURPLE) {
                        pts = 500;
                        S.purpleActive = true;
                        Sfx.purpleHit();
                    } else if (peg.type === Physics.PEG.GREEN) {
                        pts = 10;
                        triggerGuide(peg);
                        Sfx.greenHit();
                    } else {
                        Sfx.pegHit(S.comboCount);
                    }
                    // Escalate multiplier on every peg, taking the better of
                    // the orange-tier or raw-combo-tier ladders. This rewards
                    // wide chain reactions (pulsewave, lucky bounces) instead
                    // of only orange-heavy shots.
                    S.mult = comboMult(S.shotOrangeCount, S.comboCount);
                    const baseMult = S.mult * (S.purpleActive ? 2 : 1);
                    S.shotScore += pts * baseMult;
                    const col = pegColor(peg.type);
                    Particles.burst(S.particles, peg.x, peg.y, col, 6, 160);
                }
                // Final-orange slow-mo and fever.
                if (Physics.countRemainingOrange(S.world) === 1 &&
                    peg.type === Physics.PEG.ORANGE &&
                    peg.lit) {
                    S.world.slowmo = 1.1;
                }
                if (Physics.countRemainingOrange(S.world) === 0 && !S.world.feverBlasted) {
                    S.world.feverBlasted = true;
                    S.shotScore += 25000;
                    S.fx.shake = 1.0;
                    showFever('ULTRA EXTREME!');
                    Sfx.fever();
                }
            } else if (e.kind === 'wall-hit') {
                Sfx.wallHit();
            } else if (e.kind === 'catchbar-hit') {
                S.world.caughtThisShot = true;
            } else if (e.kind === 'ball-exit') {
                // If no other active ball remains, shot finishes.
                if (!Physics.hasActiveBall(S.world)) finishShot();
            }
        }
        ev.length = 0;
    }

    function comboMult(orangeCleared, comboCount) {
        const fromOrange =
            orangeCleared >= 15 ? 10 :
            orangeCleared >= 10 ? 5 :
            orangeCleared >= 6  ? 3 :
            orangeCleared >= 3  ? 2 : 1;
        const fromCombo =
            comboCount >= 30 ? 10 :
            comboCount >= 20 ? 5 :
            comboCount >= 12 ? 3 :
            comboCount >= 6  ? 2 : 1;
        return Math.max(fromOrange, fromCombo);
    }

    function showFever(text) {
        const el = document.getElementById('fever-text');
        if (!el) return;
        el.textContent = text;
        el.style.display = 'block';
        setTimeout(() => { el.style.display = 'none'; }, 1800);
    }

    function triggerGuide(peg) {
        const g = Guides.byId(S.guideId);
        g.trigger(S.world, peg);
    }

    // ----------------- Rendering -------------------------------------------
    const PEG_COLORS = {
        blue:   '#5aa6ff',
        orange: '#ff9a2a',
        green:  '#58e05a',
        purple: '#c97aff',
    };
    function pegColor(type) { return PEG_COLORS[type] || '#eee'; }

    function drawBackground(lv) {
        const [a, b] = lv.background;
        const grad = ctx.createLinearGradient(0, 0, 0, FIELD_H);
        grad.addColorStop(0, a);
        grad.addColorStop(1, b);
        ctx.fillStyle = grad;
        ctx.fillRect(0, 0, FIELD_W, FIELD_H);
        // Soft vignette.
        const v = ctx.createRadialGradient(FIELD_W/2, FIELD_H/2, FIELD_H*0.3, FIELD_W/2, FIELD_H/2, FIELD_H*0.9);
        v.addColorStop(0, 'rgba(0,0,0,0)');
        v.addColorStop(1, 'rgba(0,0,0,0.55)');
        ctx.fillStyle = v;
        ctx.fillRect(0, 0, FIELD_W, FIELD_H);
        // Subtle animated stars.
        const t = performance.now() / 1000;
        for (let i = 0; i < 40; i++) {
            const x = ((i * 193) % FIELD_W);
            const y = ((i * 89 + Math.sin(t + i) * 6) % FIELD_H);
            ctx.fillStyle = 'rgba(255,255,255,' + (0.03 + (i % 3) * 0.02) + ')';
            ctx.fillRect(x, y, 2, 2);
        }
    }

    function drawPeg(p) {
        if (p.removed) return;
        const col  = pegColor(p.type);
        const r    = Physics.PEG_RADIUS;
        const xx   = p.x, yy = p.y;
        const t    = performance.now() / 1000;

        if (p.type === 'orange' && !p.lit) {
            const pulse = 1 + Math.sin(t * 5 + p.phase) * 0.06;
            const glow = ctx.createRadialGradient(xx, yy, 0, xx, yy, r * 3);
            glow.addColorStop(0, 'rgba(255,170,60,0.45)');
            glow.addColorStop(1, 'rgba(255,170,60,0)');
            ctx.fillStyle = glow;
            ctx.beginPath();
            ctx.arc(xx, yy, r * 3 * pulse, 0, Math.PI * 2);
            ctx.fill();
        } else if (p.type === 'green' && !p.lit) {
            const glow = ctx.createRadialGradient(xx, yy, 0, xx, yy, r * 2.4);
            glow.addColorStop(0, 'rgba(90,230,100,0.4)');
            glow.addColorStop(1, 'rgba(90,230,100,0)');
            ctx.fillStyle = glow;
            ctx.beginPath();
            ctx.arc(xx, yy, r * 2.4, 0, Math.PI * 2);
            ctx.fill();
        }

        // Body — gradient ball.
        const g = ctx.createRadialGradient(xx - r*0.3, yy - r*0.3, 1, xx, yy, r);
        g.addColorStop(0, lighten(col, 0.4));
        g.addColorStop(1, darken(col, 0.25));
        ctx.fillStyle = g;
        ctx.beginPath();
        ctx.arc(xx, yy, r, 0, Math.PI * 2);
        ctx.fill();

        // Highlight dot.
        ctx.fillStyle = 'rgba(255,255,255,0.45)';
        ctx.beginPath();
        ctx.arc(xx - r*0.35, yy - r*0.35, r*0.3, 0, Math.PI * 2);
        ctx.fill();

        if (p.lit) {
            ctx.globalAlpha = 0.75;
            ctx.strokeStyle = '#fff';
            ctx.lineWidth = 2;
            ctx.beginPath();
            ctx.arc(xx, yy, r + 3, 0, Math.PI * 2);
            ctx.stroke();
            ctx.globalAlpha = 1;
        }
    }

    function drawBall(b) {
        if (!b || !b.active) return;
        const r = b.radius;
        // Trail
        ctx.globalAlpha = 0.3;
        ctx.fillStyle = '#fff';
        for (let i = 1; i <= 4; i++) {
            const t = i / 5;
            ctx.beginPath();
            ctx.arc(b.x - b.vx * t * 0.03, b.y - b.vy * t * 0.03, r * (1 - t * 0.6), 0, Math.PI * 2);
            ctx.fill();
        }
        ctx.globalAlpha = 1;

        // Shadow
        ctx.fillStyle = 'rgba(0,0,0,0.35)';
        ctx.beginPath();
        ctx.ellipse(b.x, b.y + r + 2, r * 0.9, r * 0.4, 0, 0, Math.PI * 2);
        ctx.fill();

        // Body — fire or plain
        const fireCol = b.onFire ? '#ff6833' : '#eaf1ff';
        const g = ctx.createRadialGradient(b.x - r*0.3, b.y - r*0.3, 1, b.x, b.y, r);
        g.addColorStop(0, lighten(fireCol, 0.4));
        g.addColorStop(1, b.onFire ? '#ff3300' : '#6a7698');
        ctx.fillStyle = g;
        ctx.beginPath();
        ctx.arc(b.x, b.y, r, 0, Math.PI * 2);
        ctx.fill();

        if (b.onFire) {
            const ring = ctx.createRadialGradient(b.x, b.y, r, b.x, b.y, 42);
            ring.addColorStop(0, 'rgba(255,150,60,0.5)');
            ring.addColorStop(1, 'rgba(255,150,60,0)');
            ctx.fillStyle = ring;
            ctx.beginPath();
            ctx.arc(b.x, b.y, 42, 0, Math.PI * 2);
            ctx.fill();
        }
    }

    function drawPulses(pulses) {
        if (!pulses || !pulses.length) return;
        for (const pw of pulses) {
            const t = Math.min(1, pw.age / pw.duration);
            const r = t * pw.R;
            const alpha = 1 - t;
            // Inner soft glow filling the swept area.
            const g = ctx.createRadialGradient(pw.cx, pw.cy, 0, pw.cx, pw.cy, Math.max(r, 1));
            g.addColorStop(0, 'rgba(140,255,150,0)');
            g.addColorStop(0.7, 'rgba(140,255,150,' + (0.18 * alpha) + ')');
            g.addColorStop(1, 'rgba(140,255,150,' + (0.35 * alpha) + ')');
            ctx.fillStyle = g;
            ctx.beginPath();
            ctx.arc(pw.cx, pw.cy, Math.max(r, 1), 0, Math.PI * 2);
            ctx.fill();
            // Bright leading ring.
            ctx.strokeStyle = 'rgba(180,255,180,' + (0.9 * alpha) + ')';
            ctx.lineWidth = 4;
            ctx.beginPath();
            ctx.arc(pw.cx, pw.cy, r, 0, Math.PI * 2);
            ctx.stroke();
        }
    }

    function drawCatchbar(cb) {
        const x = cb.x - cb.halfW;
        const y = cb.y;
        const w = cb.halfW * 2;
        const h = Physics.CATCHBAR_H;
        const g = ctx.createLinearGradient(0, y, 0, y + h);
        g.addColorStop(0, '#ffd870');
        g.addColorStop(1, '#b8740e');
        ctx.fillStyle = g;
        ctx.fillRect(x, y, w, h);
        ctx.fillStyle = 'rgba(255,255,255,0.4)';
        ctx.fillRect(x, y, w, 3);
        ctx.fillStyle = 'rgba(0,0,0,0.3)';
        ctx.fillRect(x, y + h - 3, w, 3);
    }

    function drawCannon() {
        const ang = S.aimAngle;
        ctx.save();
        ctx.translate(S.cannonX, S.cannonY);
        // Base plate
        ctx.fillStyle = '#1a2747';
        ctx.beginPath();
        ctx.arc(0, 0, 24, 0, Math.PI * 2);
        ctx.fill();
        ctx.strokeStyle = '#3a5299';
        ctx.lineWidth = 2;
        ctx.stroke();
        // Barrel
        ctx.rotate(ang);
        ctx.fillStyle = '#c5cde2';
        ctx.fillRect(0, -6, 30, 12);
        ctx.fillStyle = '#8a95b5';
        ctx.fillRect(0, 3, 30, 3);
        ctx.restore();
        // Preview ball in muzzle
        if (!S.shotInProgress && S.balls > 0) {
            const px = S.cannonX + Math.cos(ang) * 28;
            const py = S.cannonY + Math.sin(ang) * 28;
            ctx.fillStyle = '#eaf1ff';
            ctx.beginPath();
            ctx.arc(px, py, 7, 0, Math.PI * 2);
            ctx.fill();
        }
    }

    function drawAimGuide() {
        if (!store.get('trajectory') && !S.mirageShowing) return;
        if (S.shotInProgress) return;
        if (S.balls <= 0) return;
        // Short-range dashed aim line, or full mirage path.
        const speed = S.lastLaunchSpeed;
        const pts = [];
        if (S.mirageShowing) {
            Physics.predict(S.world, S.aimAngle, speed,
                S.cannonX + Math.cos(S.aimAngle) * 30,
                S.cannonY + Math.sin(S.aimAngle) * 30,
                2.2, pts);
        } else {
            // Short free-space arc.
            const x0 = S.cannonX + Math.cos(S.aimAngle) * 30;
            const y0 = S.cannonY + Math.sin(S.aimAngle) * 30;
            let vx = Math.cos(S.aimAngle) * speed;
            let vy = Math.sin(S.aimAngle) * speed;
            let x = x0, y = y0;
            const dt = 1 / 60;
            for (let i = 0; i < 22; i++) {
                x += vx * dt;
                y += vy * dt;
                vy += 1400 * dt; // GRAVITY constant from physics.js
                if (y > FIELD_H) break;
                // Stop at first peg
                let hitPeg = false;
                for (const p of S.world.pegs) {
                    if (p.removed) continue;
                    if (Math.hypot(p.x - x, p.y - y) < Physics.PEG_RADIUS + 6) { hitPeg = true; break; }
                }
                if (hitPeg) break;
                pts.push({ x, y });
            }
        }
        for (let i = 0; i < pts.length; i++) {
            const p = pts[i];
            const a = 1 - (i / pts.length);
            ctx.fillStyle = 'rgba(255,255,255,' + (a * 0.6).toFixed(3) + ')';
            ctx.beginPath();
            ctx.arc(p.x, p.y, 3, 0, Math.PI * 2);
            ctx.fill();
        }
    }

    function drawHud() {
        if (!S.world) return;
        const lv = Levels.LEVELS[S.levelIdx];
        Hud.text('#hud-score',  S.score + S.shotScore);
        Hud.text('#hud-balls',  S.balls);
        Hud.text('#hud-orange', Physics.countRemainingOrange(S.world));
        Hud.text('#hud-mult',   'x' + (S.mult * (S.purpleActive ? 2 : 1)));
        Hud.text('#hud-guide',  Guides.byId(S.guideId).name);
        Hud.text('#hud-level',  (S.levelIdx + 1) + ' — ' + lv.name);

        if (S.comboCount > 4) {
            const el = document.getElementById('combo-text');
            if (el) {
                el.textContent = 'COMBO ×' + S.comboCount;
                el.style.display = 'block';
            }
        } else {
            const el = document.getElementById('combo-text');
            if (el) el.style.display = 'none';
        }
    }

    // ----------------- Colour helpers --------------------------------------
    function hexToRgb(h) {
        if (h[0] === '#') h = h.slice(1);
        const n = parseInt(h, 16);
        return { r: (n >> 16) & 255, g: (n >> 8) & 255, b: n & 255 };
    }
    function rgbToHex(r, g, b) {
        const t = (v) => Math.max(0, Math.min(255, v | 0)).toString(16).padStart(2, '0');
        return '#' + t(r) + t(g) + t(b);
    }
    function lighten(h, f) {
        const c = hexToRgb(h);
        return rgbToHex(c.r + (255 - c.r) * f, c.g + (255 - c.g) * f, c.b + (255 - c.b) * f);
    }
    function darken(h, f) {
        const c = hexToRgb(h);
        return rgbToHex(c.r * (1 - f), c.g * (1 - f), c.b * (1 - f));
    }

    // ----------------- Main loop -------------------------------------------
    const loop = GameLoop.create({
        tick(dtMs) {
            const dt = Math.min(0.033, dtMs / 1000);
            if (screens.name() === 'playing' && S.world) {
                // Keyboard aiming fallback.
                if (!S.mouseOverCanvas || true) {
                    const rate = Math.PI * 0.9 * dt;
                    if (Input.down('aim_left'))  S.aimAngle -= rate;
                    if (Input.down('aim_right')) S.aimAngle += rate;
                    S.aimAngle = Math.max(0.08, Math.min(Math.PI - 0.08, S.aimAngle));
                }
                // Launch from keyboard.
                if (Input.pressed('launch')) tryLaunch();

                // Physics step. The sandbox-handle implementation in
                // physics.js handles its own substepping internally for
                // tunneling safety; we just hand it a frame dt.
                Physics.step(S.world, dt);
                drainEvents();
                Particles.step(S.particles, dt);

                if (S.fx.shake > 0) S.fx.shake = Math.max(0, S.fx.shake - dt * 2);
            }
            drawHud();
        },
        draw() {
            const dims = canvasSize();
            ctx.clearRect(0, 0, dims.w, dims.h);
            if (!S.world) return;
            const fit = fitScale(dims.w, dims.h);
            ctx.save();
            // Letterbox bars.
            ctx.fillStyle = '#04060c';
            ctx.fillRect(0, 0, dims.w, dims.h);
            // Shake
            let sx = 0, sy = 0;
            if (S.fx.shake > 0 && store.get('screenshake')) {
                sx = (Math.random() - 0.5) * 8 * S.fx.shake;
                sy = (Math.random() - 0.5) * 8 * S.fx.shake;
            }
            ctx.translate(fit.offX + sx, fit.offY + sy);
            ctx.scale(fit.scale, fit.scale);

            const lv = Levels.LEVELS[S.levelIdx];
            drawBackground(lv);
            // Top bar for cannon zone
            ctx.fillStyle = 'rgba(8, 12, 26, 0.85)';
            ctx.fillRect(0, 0, FIELD_W, Physics.FIELD_TOP);
            ctx.strokeStyle = '#23306a';
            ctx.lineWidth = 1;
            ctx.beginPath();
            ctx.moveTo(0, Physics.FIELD_TOP);
            ctx.lineTo(FIELD_W, Physics.FIELD_TOP);
            ctx.stroke();

            drawAimGuide();
            for (const p of S.world.pegs) drawPeg(p);
            drawPulses(S.world.pulses);
            drawBall(S.world.ball);
            for (const eb of S.world.extraBalls) drawBall(eb);
            drawCatchbar(S.world.catchbar);
            drawCannon();
            Particles.draw(S.particles, ctx);

            ctx.restore();
        },
    });

    // ----------------- Screens ---------------------------------------------
    function setScreenVisible(playing) {
        if (playing) Hud.show('#hud');
        else         Hud.hide('#hud');
    }

    screens.define('title', {
        enter() {
            setScreenVisible(false);
            screens.showOverlay('title');
            screens.updateSelection('title');
        },
        keydown(key) {
            screens.menuNav('title', key, (_, item) => {
                const a = item && item.dataset.action;
                if      (a === 'play')       { screens.switchTo('levels'); }
                else if (a === 'levels')     { screens.switchTo('levels'); }
                else if (a === 'highscores') { screens.switchTo('highscores'); }
                else if (a === 'howto')      { screens.switchTo('howto'); }
                else if (a === 'settings')   { screens.switchTo('settings'); }
                else if (a === 'credits')    { screens.switchTo('credits'); }
                else if (a === 'quit')       { window.close && window.close(); }
            });
        },
    });

    screens.define('levels', {
        enter() {
            setScreenVisible(false);
            const grid = document.getElementById('level-grid');
            PegScreens.renderLevelGrid(grid, Levels.LEVELS, store, (idx) => {
                S.levelIdx = idx;
                screens.switchTo('guide');
            });
            screens.showOverlay('levels');
            screens.updateSelection('levels');
        },
        keydown(key) {
            screens.menuNav('levels', key, (_, item) => {
                const a = item && item.dataset.action;
                if (a === 'back') screens.switchTo('title');
            }, { onBack: () => screens.switchTo('title') });
        },
    });

    screens.define('guide', {
        enter() {
            setScreenVisible(false);
            const root = document.getElementById('guide-cards');
            const render = () => PegScreens.renderGuideCards(root, Guides.GUIDES, S.guideId, (id) => {
                S.guideId = id;
                store.set('selectedGuide', id);
                store.save();
                render();
            });
            render();
            screens.showOverlay('guide');
            screens.updateSelection('guide');
        },
        keydown(key) {
            screens.menuNav('guide', key, (_, item) => {
                const a = item && item.dataset.action;
                if (a === 'confirm') {
                    loadLevel(S.levelIdx);
                    screens.hideOverlay();
                    setScreenVisible(true);
                    screens.switchTo('playing');
                } else if (a === 'back') {
                    screens.switchTo('levels');
                }
            }, { onBack: () => screens.switchTo('levels') });
        },
    });

    screens.define('playing', {
        enter() {
            setScreenVisible(true);
            screens.hideOverlay();
        },
        keydown(key) {
            if (key === 'Escape') {
                screens.switchTo('pause');
            }
        },
    });

    screens.define('pause', {
        enter() {
            setScreenVisible(false);
            screens.showOverlay('pause');
            screens.updateSelection('pause');
        },
        keydown(key) {
            screens.menuNav('pause', key, (_, item) => {
                const a = item && item.dataset.action;
                if      (a === 'resume')   { screens.hideOverlay(); screens.switchTo('playing'); }
                else if (a === 'restart')  { loadLevel(S.levelIdx); screens.hideOverlay(); screens.switchTo('playing'); }
                else if (a === 'settings') { screens.switchTo('settings'); }
                else if (a === 'quit')     { screens.switchTo('title'); }
            }, { onBack: () => { screens.hideOverlay(); screens.switchTo('playing'); } });
        },
    });

    screens.define('clear', {
        enter() {
            setScreenVisible(false);
            const lv = Levels.LEVELS[S.levelIdx];
            const stars = starCount(S.score, lv.stars);
            const msg = 'Level ' + (S.levelIdx + 1) + ' — ' + lv.name + '\n' +
                        'Score: ' + S.score + '\n' +
                        'Balls used: ' + (S.ballsStart - S.balls);
            document.getElementById('clear-stats').textContent = msg;
            document.getElementById('clear-stars').textContent =
                ['★','★','★'].slice(0, stars).join('') +
                ['☆','☆','☆'].slice(0, 3 - stars).join('');
            screens.showOverlay('clear');
            screens.updateSelection('clear');
        },
        keydown(key) {
            screens.menuNav('clear', key, (_, item) => {
                const a = item && item.dataset.action;
                if (a === 'next') {
                    const nxt = Math.min(S.levelIdx + 1, Levels.LEVELS.length - 1);
                    S.levelIdx = nxt;
                    screens.switchTo('guide');
                } else if (a === 'retry') {
                    loadLevel(S.levelIdx); screens.hideOverlay(); screens.switchTo('playing');
                } else if (a === 'levels') { screens.switchTo('levels'); }
            });
        },
    });

    screens.define('fail', {
        enter() {
            setScreenVisible(false);
            document.getElementById('fail-stats').textContent =
                'Cleared ' + (S.totalOrangeStart - Physics.countRemainingOrange(S.world)) +
                ' of ' + S.totalOrangeStart + ' orange pegs.\n' +
                'Score: ' + S.score;
            screens.showOverlay('fail');
            screens.updateSelection('fail');
        },
        keydown(key) {
            screens.menuNav('fail', key, (_, item) => {
                const a = item && item.dataset.action;
                if (a === 'retry') { loadLevel(S.levelIdx); screens.hideOverlay(); screens.switchTo('playing'); }
                else if (a === 'levels') { screens.switchTo('levels'); }
                else if (a === 'quit')   { screens.switchTo('title'); }
            });
        },
    });

    screens.define('highscores', {
        enter() {
            setScreenVisible(false);
            PegScreens.renderHighScores(document.getElementById('hs-list'),
                                        Levels.LEVELS, store);
            screens.showOverlay('highscores');
            screens.updateSelection('highscores');
        },
        keydown(key) {
            screens.menuNav('highscores', key,
                () => screens.switchTo('title'),
                { onBack: () => screens.switchTo('title') });
        },
    });

    screens.define('howto', {
        enter() {
            setScreenVisible(false);
            screens.showOverlay('howto');
            screens.updateSelection('howto');
        },
        keydown(key) {
            screens.menuNav('howto', key,
                () => screens.switchTo('title'),
                { onBack: () => screens.switchTo('title') });
        },
    });

    screens.define('settings', {
        enter() {
            setScreenVisible(false);
            PegScreens.renderSettings(store);
            screens.showOverlay('settings');
            screens.updateSelection('settings');
        },
        keydown(key) {
            const adjust = (delta) => {
                const items = screens.getMenuItems('settings');
                const idx = items.findIndex(el => el.classList.contains('selected'));
                const item = items[idx];
                if (!item) return;
                const setting = item.dataset.setting;
                if (!setting) return;
                if (setting === 'sfxVol' || setting === 'musicVol') {
                    const v = Math.max(0, Math.min(1, (store.get(setting) || 0) + delta * 0.1));
                    store.set(setting, v);
                } else if (setting === 'trajectory' || setting === 'screenshake') {
                    store.set(setting, !store.get(setting));
                }
                store.save();
                PegScreens.renderSettings(store);
                SFX.setSfxVol(store.get('sfxVol'));
                SFX.setMusicVol(store.get('musicVol'));
            };
            screens.menuNav('settings', key, (_, item) => {
                const a = item && item.dataset.action;
                if (a === 'back') screens.switchTo('title');
                else adjust(1);
            }, {
                onAdjust: (d) => adjust(d),
                onBack: () => screens.switchTo('title'),
            });
        },
    });

    screens.define('credits', {
        enter() {
            setScreenVisible(false);
            screens.showOverlay('credits');
            screens.updateSelection('credits');
        },
        keydown(key) {
            screens.menuNav('credits', key,
                () => screens.switchTo('title'),
                { onBack: () => screens.switchTo('title') });
        },
    });

    // Key event pump to current screen.
    window.addEventListener('keydown', (e) => {
        screens.keydown(e.key);
    });

    // ----------------- Boot ------------------------------------------------
    screens.switchTo('title');
    loop.start();

    // ----------------- Test hooks ------------------------------------------
    // Deterministic-shot helper: builds a private world, runs a fixed
    // number of ticks, returns score + counts.
    function simulateShot(angle, seed, guideId) {
        const prev = {
            worldIdx: S.levelIdx,
        };
        const w = Levels.buildLevel(S.levelIdx, seed | 0);
        Physics.launchBall(w, angle, 820, FIELD_W / 2, 64);
        const dt = 1 / 180;
        let elapsed = 0;
        let orangeCleared = 0;
        let shotScore = 0;
        let comboMultSeen = 1;
        let shotOrangeCount = 0;
        let shotComboCount = 0;
        const startOrange = Physics.countRemainingOrange(w);
        while (Physics.hasActiveBall(w) && elapsed < 12) {
            Physics.step(w, dt);
            const ev = w.scoreEvents;
            Physics.markLitFromEvents(w, ev);
            for (const e of ev) {
                if (e.kind !== 'peg-hit') continue;
                const peg = e.peg;
                if (peg._testScored) continue;
                peg._testScored = true;
                shotComboCount++;
                let pts = 10;
                if (peg.type === Physics.PEG.ORANGE) {
                    pts = 100;
                    shotOrangeCount++;
                }
                else if (peg.type === Physics.PEG.PURPLE) pts = 500;
                const m = comboMult(shotOrangeCount, shotComboCount);
                comboMultSeen = Math.max(comboMultSeen, m);
                shotScore += pts * m;
            }
            ev.length = 0;
            elapsed += dt;
        }
        Physics.sweepLit(w);
        orangeCleared = startOrange - Physics.countRemainingOrange(w);
        const result = { orangeCleared, shotScore, comboMult: comboMultSeen, elapsed, startOrange };
        Physics.destroyWorld(w);
        return result;
    }

    // Same as simulateShot but also applies to the *live* world (for
    // integration testing — fires an actual shot).
    function fireLiveShot(angle) {
        S.aimAngle = angle;
        tryLaunch();
    }

    window.__pegbounce = {
        S, store,
        Physics, Levels, Guides, Particles,
        loadLevel,
        fireLiveShot,
        tryLaunch,
        simulateShot,
        stepPhysics(dt) { if (S.world) Physics.step(S.world, dt); drainEvents(); },
        findPegs()      { return S.world ? S.world.pegs : []; },
        remainingOrange(){ return S.world ? Physics.countRemainingOrange(S.world) : 0; },
        currentScore()   { return S.score + S.shotScore; },
        screens, canvas, ctx,
        setGuide(id) { S.guideId = id; store.set('selectedGuide', id); store.save(); },
        forceClear() { S.levelClearTriggered = true; onLevelClear(); },
    };

    console.log('Pegbounce loaded.');
})();
