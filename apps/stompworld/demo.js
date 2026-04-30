// demo.js — scripted "ending" scene that runs after the agent has been
// trained. Plays back a single full level run with two distinct phases:
//
//   1. Network mode: greedy argmax over the trained policy drives movement
//      from spawn until the agent overlaps the beam pickup. Auto-fire
//      (built into sim.js) handles enemies and destructible terrain.
//   2. Scripted mode: once hasWeapon is true the network is dropped. A
//      tiny hand-rolled controller walks the player back to the spawn
//      column (jumping over pits / walls), then turns around and walks
//      to the flag. Auto-fire continues clearing everything ahead.
//
// Render is a thin clone of Training.draw — the sim runs on main, no
// workers, no MCTS. If `ckpt/best.bin` is missing the demo bounces back
// to the title.

(function (global) {
    'use strict';

    const TILE = 32;
    const VIEW_W = 800;
    const VIEW_H = 576;
    const NN = bro && bro.ai && bro.ai.game && bro.ai.game.nn;

    // One sim decision = FRAME_SKIP physics ticks (~67 ms). Match the
    // training cadence so the demo runs at human-comfortable speed.
    const DECISION_INTERVAL_MS = 1000 / 60 * 4;

    function scriptedMove(sim, dir) {
        const p = sim.player;
        const onGround = !!p.onGround;
        const coyoteHot = (p.coyote || 0) > 10;
        const tm = sim.tilemap;
        const pCol = Math.floor((p.x + p.w / 2) / TILE);
        const pRow = Math.floor((p.y + p.h / 2) / TILE);
        const footRow = Math.floor((p.y + p.h + 2) / TILE);
        function solid(c, r) {
            if (c < 0 || c >= tm.cols || r < 0 || r >= tm.rows) return false;
            return !!tm.solidAt(c, r);
        }
        const pit  = !solid(pCol + dir, footRow);
        const wall = solid(pCol + dir, pRow);
        const head = solid(pCol + dir, pRow - 1);
        const triggerJump = pit || wall || head;
        if (triggerJump && (onGround || coyoteHot)) return dir > 0 ? 5 : 4;
        if (!onGround && p.vy < -50)                return dir > 0 ? 5 : 4;
        return dir > 0 ? 2 : 1;
    }

    function argmax(arr) {
        let am = 0;
        for (let i = 1; i < arr.length; i++) if (arr[i] > arr[am]) am = i;
        return am;
    }

    function create(opts) {
        opts = opts || {};
        const ctx = opts.ctx;
        const Art = opts.Art;
        const Camera2D = opts.Camera2D;
        const Game = opts.Game;        // shares tunables (BEAM_TTL_MS, etc.)

        let sim = null;
        let cam = null;
        let net = null;
        let xT  = null;
        let lgT = null;
        let weightsLoaded = false;
        let missingCkpt = false;

        // Phases:
        //   'network'    — policy net argmax until pickup is collected.
        //   'backtrack'  — scripted left until player x crosses spawnX.
        //   'toflag'     — scripted right until done.
        //   'finished'   — sim.done; freeze-frame until user backs out.
        let phase = 'network';
        let spawnX0 = 0;
        let lastDecisionMs = 0;
        let beams = [];                // [{x0,y0,x1,y1,ttl,ttlMax}]
        let pickupAnimT = 0;
        let lvlPickup = null;
        let lvlFlag = null;
        let endReason = '';

        function loadCheckpoint() {
            try {
                const fs = require('fs');
                const bytes = fs.readFileSync('apps/stompworld/ckpt/best.bin');
                net.load(new Uint8Array(bytes));
                weightsLoaded = true;
            } catch (_) {
                missingCkpt = true;
            }
        }

        function start() {
            const lvl = Level.buildLevel({ tileSize: TILE, destructible: true });
            lvlPickup = lvl.pickup;
            lvlFlag   = lvl.flag;
            sim = SwSim.create({
                tilemap: lvl.tilemap,
                spawn: lvl.spawn,
                stompers: lvl.stompers, flyers: lvl.flyers,
                flag: lvl.flag, pickup: lvl.pickup,
                timeLimit: 600,
                stallDecisions: 0,         // no stall-out — this is a demo
            });
            cam = Camera2D.create({
                viewW: VIEW_W, viewH: VIEW_H,
                levelW: lvl.tilemap.widthPx,
                levelH: lvl.tilemap.heightPx,
                deadzoneW: 120, deadzoneH: 1024,
            });
            cam.snapTo(sim.player.x + sim.player.w / 2, VIEW_H / 2);

            net = NN.createPolicyValueNet({
                inDim: SwAgentObs.OBS_DIM,
                hidden: [128, 128], valueHidden: 64,
                headSizes: SwSim.HEAD_SIZES,
                seed: 0xA11CE5n,
            });
            xT  = NN.createTensor(SwAgentObs.OBS_DIM);
            lgT = NN.createTensor(SwSim.PER_HEAD_TOTAL);
            weightsLoaded = false;
            missingCkpt = false;
            loadCheckpoint();

            phase = missingCkpt ? 'finished' : 'network';
            spawnX0 = sim.player.x;
            lastDecisionMs = 0;
            beams = [];
            pickupAnimT = 0;
            endReason = '';
        }

        function decideAction() {
            if (phase === 'network') {
                xT.fromArray(SwAgentObs.build(sim));
                net.forward(xT, lgT);
                return argmax(lgT.toArray());
            }
            if (phase === 'backtrack') return scriptedMove(sim, -1);
            if (phase === 'toflag')    return scriptedMove(sim,  1);
            return 0;
        }

        function step() {
            const a = decideAction();
            const out = sim.step(a);
            if (sim.tilemap.commitOverlays) sim.tilemap.commitOverlays();

            // Pull any beams the auto-fire produced this decision into our
            // ttl-decayed visual buffer.
            for (const b of sim.recentBeams) {
                beams.push({
                    x0: b.x0, y0: b.y0, x1: b.x1, y1: b.y1,
                    ttl: Game.BEAM_TTL_MS, ttlMax: Game.BEAM_TTL_MS,
                });
            }

            // Phase transitions.
            if (phase === 'network' && sim.hasWeapon) {
                phase = 'backtrack';
            } else if (phase === 'backtrack' && sim.player.x <= spawnX0 + 2) {
                phase = 'toflag';
            }
            if (out.done) {
                endReason = sim.won ? 'flag' : (sim.timeLeft <= 0 ? 'timeout' : 'death');
                phase = 'finished';
            }
        }

        function update(dt) {
            if (!sim) return;
            if (phase === 'finished') return;
            pickupAnimT += dt;
            // Decay beams.
            if (beams.length) {
                for (const b of beams) b.ttl -= dt;
                beams = beams.filter((b) => b.ttl > 0);
            }
            lastDecisionMs += dt;
            // Catch up if we fell behind (e.g. window-paused), but cap so we
            // don't burn the frame budget on a long pause.
            let budget = 4;
            while (lastDecisionMs >= DECISION_INTERVAL_MS && budget-- > 0) {
                lastDecisionMs -= DECISION_INTERVAL_MS;
                step();
                if (phase === 'finished') break;
            }
            cam.follow(sim.player.x + sim.player.w / 2, VIEW_H / 2);
        }

        function drawBeams() {
            for (const b of beams) {
                const a = Math.max(0, b.ttl / b.ttlMax);
                ctx.save();
                ctx.lineCap = 'round';
                ctx.strokeStyle = 'rgba(255, 220, 80, ' + (a * 0.85).toFixed(3) + ')';
                ctx.lineWidth = Game.BEAM_THICKNESS + 6;
                ctx.beginPath();
                ctx.moveTo(b.x0 - cam.x, b.y0 - cam.y);
                ctx.lineTo(b.x1 - cam.x, b.y1 - cam.y);
                ctx.stroke();
                ctx.strokeStyle = 'rgba(255, 255, 240, ' + a.toFixed(3) + ')';
                ctx.lineWidth = Math.max(1, Game.BEAM_THICKNESS - 4);
                ctx.beginPath();
                ctx.moveTo(b.x0 - cam.x, b.y0 - cam.y);
                ctx.lineTo(b.x1 - cam.x, b.y1 - cam.y);
                ctx.stroke();
                ctx.restore();
            }
        }

        function draw() {
            if (missingCkpt) {
                drawNoCkpt();
                return;
            }
            if (!sim) return;
            sim.tilemap.draw(ctx, cam.x, cam.y, VIEW_W, VIEW_H);
            if (lvlFlag) {
                Art.drawFlag(ctx,
                    lvlFlag.x - cam.x,
                    lvlFlag.y - cam.y);
            }
            if (lvlPickup && !sim.pickupCollected) {
                Art.drawPickup(ctx,
                    lvlPickup.x - cam.x,
                    lvlPickup.y - cam.y,
                    pickupAnimT);
            }
            for (const s of sim.stompers) {
                if (!cam.visible(s.x, s.y, s.w, s.h)) continue;
                const fr = !s.alive ? 2 : (Math.floor((s.animT || 0) / 200) % 2);
                Art.drawStomper(ctx,
                    s.x - cam.x,
                    s.y - cam.y, fr);
            }
            for (const f of sim.flyers) {
                if (!f.alive) continue;
                if (!cam.visible(f.x, f.y, f.w, f.h)) continue;
                const fr = (Math.floor((f.animT || 0) / 150) % 2);
                Art.drawFlyer(ctx,
                    f.x - cam.x,
                    f.y - cam.y, fr, (f.vx || 0) > 0);
            }
            const p = sim.player;
            let frame = 0;
            if (!p.onGround) frame = 3;
            else if (Math.abs(p.vx) > 8) frame = 1 + (((sim.tick / 8) | 0) % 2);
            Art.drawHero(ctx,
                p.x - cam.x,
                p.y - cam.y - 2,
                frame, p.facing < 0);
            drawBeams();
            drawHud();
        }

        function drawHud() {
            const lines = [
                'AI DEMO — Esc to exit',
                'phase: ' + phase + (endReason ? ' (' + endReason + ')' : ''),
                'hasWeapon: ' + (sim.hasWeapon ? 'yes' : 'no')
                    + '   x=' + (sim.player.x | 0)
                    + '   col=' + Math.floor(sim.player.x / TILE),
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
        }

        function drawNoCkpt() {
            ctx.save();
            ctx.fillStyle = '#000';
            ctx.fillRect(0, 0, VIEW_W, VIEW_H);
            ctx.fillStyle = '#fff';
            ctx.font = 'bold 22px monospace';
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.fillText('No checkpoint found', VIEW_W / 2, VIEW_H / 2 - 18);
            ctx.font = '14px monospace';
            ctx.fillStyle = '#cde';
            ctx.fillText('Train the AI first (apps/stompworld/ckpt/best.bin)',
                VIEW_W / 2, VIEW_H / 2 + 10);
            ctx.fillText('Esc to return to the title screen',
                VIEW_W / 2, VIEW_H / 2 + 36);
            ctx.restore();
        }

        function stop() {
            sim = null;
            cam = null;
            net = null;
            xT = lgT = null;
            beams = [];
        }

        return { start, stop, update, draw, get phase() { return phase; } };
    }

    global.SwDemo = { create };
})(typeof window !== 'undefined' ? window : globalThis);
