// inspect_ckpt.js — load a saved PolicyValueNet snapshot and dump what it
// thinks at a few interesting positions. Run via:
//   bro-headless apps/stompworld inspect_ckpt.js
//
// The app's index.html has already loaded sim/agent/level/etc. by the time
// this script executes, so we reuse the same globals. We *don't* need the
// trainer worker — pure inference.

(function () {
    'use strict';

    const fs = require('fs');
    const TILE = 32;
    const NN = bro.ai.game.nn;

    const path = 'apps/stompworld/ckpt2/best.bin';
    const meta = JSON.parse(fs.readFileSync('apps/stompworld/ckpt2/best.json', 'utf-8'));
    const bytes = new Uint8Array(fs.readFileSync(path));

    console.log('='.repeat(72));
    console.log('checkpoint:', path, '  (' + bytes.length + ' bytes)');
    console.log('  meanReturn(20):', meta.meanReturn.toFixed(4),
                '  episode:', meta.episode,
                '  netVersion:', meta.netVersion,
                '  spawnCol:', meta.spawnCol);
    console.log('='.repeat(72));

    // Build a sim shaped exactly like the worker's.
    const lvl = Level.load({ tileSize: TILE });
    let spawn = { x: 0, y: 0 };
    const stomperTemplates = [];
    let flag = null;
    for (const e of lvl.entities) {
        if (e.kind === 'player') { spawn.x = e.x; spawn.y = e.y; }
        else if (e.kind === 'stomper') {
            stomperTemplates.push({
                x: e.x + 2, y: (e.row + 1) * TILE - 24,
                w: 28, h: 24, vx: -50, vy: 0,
                onGround: false, alive: true, squashTimer: 0, animT: 0,
            });
        } else if (e.kind === 'flag') {
            flag = { x: e.x, w: 32, h: 96, y: e.row * TILE - flag_h() + TILE };
        }
    }
    function flag_h() { return 96; }
    const sim = SwSim.create({
        tilemap: lvl.tilemap, spawn, stompers: stomperTemplates, flag,
        timeLimit: 300,
    });

    // Build matching net, load weights.
    const net = NN.createPolicyValueNet({
        inDim: SwAgentObs.OBS_DIM,
        hidden: [128, 128], valueHidden: 64,
        headSizes: SwSim.HEAD_SIZES,
        seed: 0xA11CE5n,
    });
    net.load(bytes);

    const xT  = NN.createTensor(SwAgentObs.OBS_DIM);
    // Per-head concatenated logits (29 = 7+13+9), not flat (819).
    const lgT = NN.createTensor(SwSim.PER_HEAD_TOTAL);

    // Per-head softmax over a slice [off, off+size). Returns a Float32Array.
    function softmaxSlice(arr, off, size) {
        let m = -Infinity;
        for (let i = 0; i < size; i++) if (arr[off + i] > m) m = arr[off + i];
        const out = new Float32Array(size);
        let s = 0;
        for (let i = 0; i < size; i++) { out[i] = Math.exp(arr[off + i] - m); s += out[i]; }
        for (let i = 0; i < size; i++) out[i] /= s;
        return out;
    }
    // Argmax flat action: per-head argmax, then encode (h0, h1, h2) → flat.
    // For movement actions (h0 != 6) the fire-target heads collapse to 0.
    function argmaxFlat(logits) {
        const HS = SwSim.HEAD_SIZES, HO = SwSim.HEAD_OFFSETS;
        const heads = [0, 0, 0];
        for (let k = 0; k < 3; k++) {
            let best = 0, bv = -Infinity;
            for (let i = 0; i < HS[k]; i++) {
                const v = logits[HO[k] + i];
                if (v > bv) { bv = v; best = i; }
            }
            heads[k] = best;
        }
        if (heads[0] !== SwSim.ACT_FIRE) { heads[1] = 0; heads[2] = 0; }
        return heads[0] * 13 * 9 + heads[1] * 9 + heads[2];
    }
    function sampleFlat(logits, rng) {
        const HS = SwSim.HEAD_SIZES, HO = SwSim.HEAD_OFFSETS;
        const heads = [0, 0, 0];
        for (let k = 0; k < 3; k++) {
            const probs = softmaxSlice(logits, HO[k], HS[k]);
            let r = rng(), acc = 0, h = 0;
            for (let i = 0; i < HS[k]; i++) { acc += probs[i]; if (r <= acc) { h = i; break; } }
            heads[k] = h;
        }
        if (heads[0] !== SwSim.ACT_FIRE) { heads[1] = 0; heads[2] = 0; }
        return heads[0] * 13 * 9 + heads[1] * 9 + heads[2];
    }

    function inspectAt(label, col, opts) {
        opts = opts || {};
        sim.setSpawn(col * TILE + 2, spawn.y - 4);
        sim.reset();
        // Optional: drift the player up off the ground a tick or two so we
        // can probe airborne states too.
        for (let i = 0; i < (opts.warmupSteps || 0); i++) sim.step(opts.warmupAction || 0);

        const obs = SwAgentObs.build(sim);
        xT.fromArray(obs);
        const value = net.forward(xT, lgT);
        const logits = lgT.toArray();
        const h0probs = softmaxSlice(logits, SwSim.HEAD_OFFSETS[0], SwSim.HEAD_SIZES[0]);

        const names = ['idle', 'L  ', 'R  ', 'J  ', 'JL ', 'JR ', 'FIRE'];
        const p = sim.player;
        console.log(
            '  col=' + String(col).padStart(3) +
            ' (x=' + p.x.toFixed(0).padStart(4) + ', y=' + p.y.toFixed(0).padStart(3) +
            ', og=' + (p.onGround ? '1' : '0') + ')   ' +
            'V=' + value.toFixed(3).padStart(7) +
            '   ' + label
        );
        let line = '    ';
        for (let i = 0; i < h0probs.length; i++) {
            line += names[i] + '=' + h0probs[i].toFixed(3) + '  ';
        }
        let am = 0;
        for (let i = 1; i < h0probs.length; i++) if (h0probs[i] > h0probs[am]) am = i;
        line += '  → ' + names[am].trim();
        console.log(line);
    }

    console.log('\nPolicy & value at key world columns:');
    console.log('(V = value head ∈ [-1,1]; closer to +1 = "I expect to flag from here")\n');

    inspectAt('intro flat',         2);
    inspectAt('approaching 1st gap',12);
    inspectAt('after 1st gap',      18);
    inspectAt('between pipes',      33);
    inspectAt('past pipes',         38);
    inspectAt('edge of 2nd gap',    44);
    inspectAt('mid 2nd gap (air)',  46, { warmupSteps: 2, warmupAction: 5 });
    inspectAt('past 2nd gap',       50);
    inspectAt('floating platform',  56);
    inspectAt('approaching 3rd gap',72);
    inspectAt('past 3rd gap',       78);
    inspectAt('long flat',          85);
    inspectAt('staircase base',     99);
    inspectAt('staircase top',      105);
    inspectAt('near flag',          115);

    // Greedy (argmax-policy) rollouts from every curriculum spawn.
    console.log('\nGreedy rollouts (no MCTS, just argmax of policy):');
    function rollout(col) {
        sim.setSpawn(col * TILE + 2, spawn.y - 4);
        sim.reset();
        let totalR = 0, decisions = 0, maxX = sim.player.x, stomps = 0;
        const startScore = sim.score;
        for (let t = 0; t < 600; t++) {
            const obs = SwAgentObs.build(sim);
            xT.fromArray(obs);
            net.forward(xT, lgT);
            const am = argmaxFlat(lgT.toArray());
            const out = sim.step(am);
            totalR += out.reward; decisions++;
            if (sim.player.x > maxX) maxX = sim.player.x;
            if (out.done) break;
        }
        const reason = sim.won ? 'flag' : sim.stalledOut ? 'stall' : sim.timeLeft <= 0 ? 'timeout' : 'death';
        stomps = ((sim.score - startScore) - (sim.won ? 1000 : 0)) / 100;
        console.log('  spawn col=' + String(col).padStart(3) +
                    '  decisions=' + String(decisions).padStart(3) +
                    '  maxCol=' + String(Math.floor(maxX/TILE)).padStart(3) +
                    '  stomps=' + stomps +
                    '  R=' + totalR.toFixed(2).padStart(6) +
                    '  ' + reason);
    }
    [2, 30, 50, 80, 95].forEach(rollout);

    // Stochastic rollouts (sample from policy) to gauge variance.
    console.log('\nStochastic rollouts (sample from policy, 5 each):');
    function sampledRollout(col) {
        let flags = 0, totalDecisions = 0;
        let bestX = 0;
        for (let trial = 0; trial < 5; trial++) {
            sim.setSpawn(col * TILE + 2, spawn.y - 4);
            sim.reset();
            let seed = (trial + 1) * 7919;
            function rng() { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 0x100000000; }
            for (let t = 0; t < 600; t++) {
                const obs = SwAgentObs.build(sim);
                xT.fromArray(obs);
                net.forward(xT, lgT);
                const a = sampleFlat(lgT.toArray(), rng);
                const out = sim.step(a);
                if (sim.player.x > bestX) bestX = sim.player.x;
                if (out.done) break;
            }
            if (sim.won) flags++;
            totalDecisions += sim.tick;
        }
        console.log('  spawn col=' + String(col).padStart(3) +
                    '  flag rate=' + flags + '/5' +
                    '  bestX across trials=' + bestX.toFixed(0) +
                    ' (col ' + Math.floor(bestX/TILE) + ')');
    }
    [2, 30, 50, 80].forEach(sampledRollout);

    console.log('\n(headless inspector finished — exit any time)');
})();
