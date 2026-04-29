// verify_physics.js — direct physics check at jumpVel=-800.
// Manually script exact-timing jumps from each gap edge to confirm the
// jumps are actually possible.

(function () {
    'use strict';
    const TILE = 32;

    const lvl = Level.load({ tileSize: TILE });
    let spawn = { x: 0, y: 0 };
    const stomperTemplates = [];
    const flyerTemplates = [];
    let flag = null;
    for (const e of lvl.entities) {
        if (e.kind === 'player') { spawn.x = e.x; spawn.y = e.y; }
        else if (e.kind === 'stomper') {
            stomperTemplates.push({
                x: e.x + 2, y: (e.row + 1) * TILE - 24,
                w: 28, h: 24, vx: -50, vy: 0,
                onGround: false, alive: true, squashTimer: 0, animT: 0,
            });
        } else if (e.kind === 'flyer' || e.kind === 'flyer_bob') {
            const bob = e.kind === 'flyer_bob';
            const cx = e.col * TILE + TILE / 2;
            const cy = e.row * TILE + TILE / 2;
            flyerTemplates.push({
                x: cx - 12, y: cy - 8, w: 24, h: 16,
                vx: -80, vy: 0,
                spawnX: cx - 12, spawnY: cy - 8,
                patrolRange: 96,
                bobAmp: bob ? 32 : 0, bobFreq: bob ? Math.PI : 0,
                bobT: 0, animT: 0,
            });
        } else if (e.kind === 'flag') {
            flag = { x: e.x, w: 32, h: 96, y: e.row * TILE - 96 + TILE };
        }
    }
    const sim = SwSim.create({
        tilemap: lvl.tilemap, spawn,
        stompers: stomperTemplates, flyers: flyerTemplates, flag,
        timeLimit: 300,
    });

    // Run R to reach takeoff col, then JR for one decision, then R repeatedly.
    function tryJump(spawnCol, runUntilCol, jumpsToFire) {
        sim.setSpawn(spawnCol * TILE + 2, spawn.y - 4);
        sim.reset();
        // Run R until at the desired takeoff column.
        for (let t = 0; t < 200; t++) {
            const cur = Math.floor(sim.player.x / TILE);
            if (cur >= runUntilCol) break;
            sim.step(2);   // 2 = right (movement-only action space)
            if (!sim.alive) break;
        }
        if (!sim.alive) return { ok: false, where: 'pre-jump death', col: Math.floor(sim.player.x / TILE) };
        // Fire jump-right for `jumpsToFire` decisions, then R repeatedly.
        let maxCol = Math.floor(sim.player.x / TILE);
        for (let t = 0; t < 60; t++) {
            const a = t < jumpsToFire ? 5 : 2;   // 5 = jump-right, 2 = right
            const out = sim.step(a);
            const c = Math.floor(sim.player.x / TILE);
            if (c > maxCol) maxCol = c;
            if (out.done) {
                const reason = sim.won ? 'flag' : sim.timeLeft <= 0 ? 'timeout' : 'death';
                return { ok: sim.won, where: reason, col: maxCol };
            }
        }
        return { ok: true, where: 'survived 60 decisions', col: maxCol };
    }

    console.log('=== gap-by-gap timing test (jumpVel=-800) ===\n');
    function fmt(r) { return 'ok=' + r.ok + ' col=' + r.col + ' (' + r.where + ')'; }
    console.log('Gap 1: cols 13-15 (3-tile)');
    for (const n of [3, 5, 7]) console.log('   takeoff col 12, hold JR ' + n + ' decisions:', fmt(tryJump(2,  12, n)));
    console.log('Gap 2: cols 45-49 (5-tile, with brick at col 47 row 12)');
    for (const n of [3, 5, 7, 10]) console.log('   takeoff col 44, hold JR ' + n + ' decisions:', fmt(tryJump(38, 44, n)));
    console.log('Gap 3: cols 73-77 (5-tile, with brick at col 75 row 12)');
    for (const n of [3, 5, 7, 10]) console.log('   takeoff col 72, hold JR ' + n + ' decisions:', fmt(tryJump(65, 72, n)));

    // End-to-end: BC heuristic from each spawn column.
    console.log('\n=== BC heuristic end-to-end (full level w/ stompers) ===');
    function runHeuristic(col, label) {
        sim.setSpawn(col * TILE + 2, spawn.y - 4);
        sim.reset();
        let maxCol = col;
        let totalR = 0;
        let stomps = 0;
        const startScore = sim.score;
        for (let t = 0; t < 600; t++) {
            const a = SwBcWarmup.heuristicAction(sim, () => 0.99);
            const out = sim.step(a);
            const c = Math.floor(sim.player.x / TILE);
            if (c > maxCol) maxCol = c;
            totalR += out.reward;
            if (out.done) {
                const reason = sim.won ? 'flag' : sim.timeLeft <= 0 ? 'timeout' : 'death';
                stomps = ((sim.score - startScore) - (sim.won ? 1000 : 0)) / 100;
                console.log('  spawn=' + String(col).padStart(3) +
                            '  maxCol=' + String(maxCol).padStart(3) +
                            '  decisions=' + String(t+1).padStart(3) +
                            '  totalR=' + totalR.toFixed(2).padStart(6) +
                            '  stomps=' + stomps +
                            '  ' + reason +
                            '  ' + (label || ''));
                return;
            }
        }
        console.log('  spawn=' + col + '  maxCol=' + maxCol + '  maxsteps  ' + (label || ''));
    }
    runHeuristic(2);
    runHeuristic(38);
    runHeuristic(50);
    runHeuristic(78);
    runHeuristic(95);
    runHeuristic(100);
    runHeuristic(110);

    // Probe many warmup spawn candidates to find one with high success
    // under the *real* stochastic RNG (2% random action injection).
    console.log('\n=== flag rate per warmup spawn, real RNG (50 trials each) ===');
    for (const c of [50, 65, 70, 78, 85, 95, 100, 105, 110]) {
        let f = 0;
        for (let i = 0; i < 50; i++) {
            let seed = (i + 1) * 0xBC51A57E;
            const rng = () => {
                seed = (seed + 0x6D2B79F5) >>> 0;
                let t = seed;
                t = Math.imul(t ^ (t >>> 15), t | 1);
                t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
                return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
            };
            sim.setSpawn(c * TILE + 2, spawn.y - 4);
            sim.reset();
            for (let t = 0; t < 600; t++) {
                const obs = SwAgentObs.build(sim);
                const a = SwBcWarmup.heuristicAction(sim, rng);
                const out = sim.step(a);
                if (out.done) break;
            }
            if (sim.won) f++;
        }
        console.log('  col=' + String(c).padStart(3) + ' → ' + f + '/50');
    }

    // Flyer-trial: try col 78 ten times to estimate flag rate.
    console.log('\n=== flag rate from col 78 (10 trials) ===');
    let flags78 = 0;
    for (let i = 0; i < 10; i++) {
        sim.setSpawn(78 * TILE + 2, spawn.y - 4);
        sim.reset();
        for (let t = 0; t < 600; t++) {
            const a = SwBcWarmup.heuristicAction(sim, () => 0.99);
            const out = sim.step(a);
            if (out.done) break;
        }
        if (sim.won) flags78++;
    }
    console.log('  ' + flags78 + '/10 flags from col 78');

    // Trace heuristic walking under col 6 row-11 flyer.
    console.log('\n=== heuristic at col 6 flyer (the live agent dies here) ===');
    sim.setSpawn(2 * TILE + 2, spawn.y - 4);
    sim.reset();
    for (let t = 0; t < 18; t++) {
        const obs = SwAgentObs.build(sim);
        const a = SwBcWarmup.heuristicAction(sim, () => 0.99);
        const f0v = obs[63] > 0.5;
        const f0dx = obs[64].toFixed(2);
        const f0dy = obs[65].toFixed(2);
        const p = sim.player;
        const c = Math.floor((p.x + p.w/2) / TILE);
        console.log('  t=' + t + ' col=' + c + ' og=' + (p.onGround?1:0) +
                    ' flyer0:[v=' + (f0v?1:0) + ',dx=' + f0dx + ',dy=' + f0dy + ']' +
                    ' → ' + ['idle','L','R','J','JL','JR'][a]);
        const out = sim.step(a);
        if (out.done) { console.log('  done won=' + sim.won); break; }
    }

    // Trace spawn=78 with flyer state.
    console.log('\n=== trace spawn=78 (heuristic, watch flyer interaction) ===');
    sim.setSpawn(78 * TILE + 2, spawn.y - 4);
    sim.reset();
    for (let t = 0; t < 25; t++) {
        const obs = SwAgentObs.build(sim);
        const a = SwBcWarmup.heuristicAction(sim, () => 0.99);
        const p = sim.player;
        const pcol = Math.floor((p.x + p.w/2) / TILE);
        // Find flyer at col 86 if visible
        const f = sim.flyers && sim.flyers.find(fl => Math.abs(fl.spawnX - 2752) < 20);
        const fInfo = f ? ' fly:x=' + f.x.toFixed(0) + ',y=' + f.y.toFixed(0) + ',vx=' + f.vx.toFixed(0) : '';
        console.log('  t=' + String(t).padStart(2) +
                    ' x=' + p.x.toFixed(0).padStart(4) + ' y=' + p.y.toFixed(0).padStart(3) +
                    ' col=' + String(pcol).padStart(3) +
                    ' a=' + ['idle','L','R','J','JL','JR'][a] +
                    fInfo);
        const out = sim.step(a);
        if (out.done) { console.log('  done won=' + sim.won); break; }
    }

    // Trace spawn=2 decision by decision.
    console.log('\n=== trace spawn=2 ===');
    sim.setSpawn(2 * TILE + 2, spawn.y - 4);
    sim.reset();
    for (let t = 0; t < 35; t++) {
        const obs = SwAgentObs.build(sim);
        const a = SwBcWarmup.heuristicAction(sim, () => 0.99);
        const p = sim.player;
        const pcol = Math.floor((p.x + p.w/2) / TILE);
        const pitAhead = !(obs[8 + 3*8 + 2] > 0.5);
        const sValid = obs[48] > 0.5;
        const sDxN = obs[49];
        const names = ['idle','L','R','J','JL','JR'];
        console.log('  t=' + String(t).padStart(2) +
                    ' x=' + p.x.toFixed(0).padStart(4) + ' y=' + p.y.toFixed(0).padStart(3) +
                    ' vx=' + p.vx.toFixed(0).padStart(4) + ' vy=' + p.vy.toFixed(0).padStart(4) +
                    ' og=' + (p.onGround?1:0) +
                    ' col=' + String(pcol).padStart(2) +
                    ' pit=' + (pitAhead?'1':'0') +
                    ' sDxN=' + (sValid?sDxN.toFixed(2):'-') +
                    ' → ' + names[a]);
        const out = sim.step(a);
        if (out.done) {
            console.log('  done: won=' + sim.won + ' alive=' + sim.alive);
            break;
        }
    }
})();
