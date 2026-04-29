// inspect_ckpt.js — load a saved PolicyValueNet snapshot and dump what it
// thinks at a few interesting positions. Run via:
//   bro-headless apps/stompworld inspect_ckpt.js
//
// The app's index.html has already loaded sim/agent/level/etc. by the time
// this script executes, so we reuse the same globals. We don't need the
// trainer worker — pure inference.

(function () {
    'use strict';

    const fs = require('fs');
    const TILE = 32;
    const NN = bro.ai.game.nn;

    const path = 'apps/stompworld/ckpt/best.bin';
    const meta = JSON.parse(fs.readFileSync('apps/stompworld/ckpt/best.json', 'utf-8'));
    const bytes = new Uint8Array(fs.readFileSync(path));

    console.log('='.repeat(72));
    console.log('checkpoint:', path, '  (' + bytes.length + ' bytes)');
    console.log('  meanReturn(20):', meta.meanReturn.toFixed(4),
                '  episode:', meta.episode,
                '  netVersion:', meta.netVersion);
    console.log('='.repeat(72));

    const lvl = Level.buildLevel({ tileSize: TILE, destructible: true });
    const sim = SwSim.create({
        tilemap: lvl.tilemap, spawn: lvl.spawn,
        stompers: lvl.stompers, flyers: lvl.flyers,
        flag: lvl.flag, pickup: lvl.pickup,
        timeLimit: 300,
    });
    const baseSpawnY = lvl.spawn.y;

    const net = NN.createPolicyValueNet({
        inDim: SwAgentObs.OBS_DIM,
        hidden: [128, 128], valueHidden: 64,
        headSizes: SwSim.HEAD_SIZES,
        seed: 0xA11CE5n,
    });
    net.load(bytes);

    const xT  = NN.createTensor(SwAgentObs.OBS_DIM);
    const lgT = NN.createTensor(SwSim.PER_HEAD_TOTAL);   // 6 movement logits

    function softmax(arr) {
        let m = -Infinity;
        for (let i = 0; i < arr.length; i++) if (arr[i] > m) m = arr[i];
        const out = new Float32Array(arr.length);
        let s = 0;
        for (let i = 0; i < arr.length; i++) { out[i] = Math.exp(arr[i] - m); s += out[i]; }
        for (let i = 0; i < arr.length; i++) out[i] /= s;
        return out;
    }
    function argmax(arr) {
        let am = 0;
        for (let i = 1; i < arr.length; i++) if (arr[i] > arr[am]) am = i;
        return am;
    }
    function sample(probs, rng) {
        let r = rng(), acc = 0;
        for (let i = 0; i < probs.length; i++) { acc += probs[i]; if (r <= acc) return i; }
        return probs.length - 1;
    }

    function inspectAt(label, col, opts) {
        opts = opts || {};
        sim.setSpawn(col * TILE + 2, baseSpawnY - 4);
        sim.reset();
        for (let i = 0; i < (opts.warmupSteps || 0); i++) sim.step(opts.warmupAction || 0);

        const obs = SwAgentObs.build(sim);
        xT.fromArray(obs);
        const value = net.forward(xT, lgT);
        const probs = softmax(lgT.toArray());

        const names = ['idle', 'L  ', 'R  ', 'J  ', 'JL ', 'JR '];
        const p = sim.player;
        console.log(
            '  col=' + String(col).padStart(3) +
            ' (x=' + p.x.toFixed(0).padStart(4) + ', y=' + p.y.toFixed(0).padStart(3) +
            ', og=' + (p.onGround ? '1' : '0') + ')   ' +
            'V=' + value.toFixed(3).padStart(7) +
            '   ' + label
        );
        let line = '    ';
        for (let i = 0; i < probs.length; i++) {
            line += names[i] + '=' + probs[i].toFixed(3) + '  ';
        }
        line += '  → ' + names[argmax(probs)].trim();
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

    console.log('\nGreedy rollouts (no MCTS, just argmax of policy):');
    function rollout(col) {
        sim.setSpawn(col * TILE + 2, baseSpawnY - 4);
        sim.reset();
        let totalR = 0, decisions = 0, maxX = sim.player.x;
        const startScore = sim.score;
        for (let t = 0; t < 600; t++) {
            const obs = SwAgentObs.build(sim);
            xT.fromArray(obs);
            net.forward(xT, lgT);
            const am = argmax(lgT.toArray());
            const out = sim.step(am);
            totalR += out.reward; decisions++;
            if (sim.player.x > maxX) maxX = sim.player.x;
            if (out.done) break;
        }
        const reason = sim.won ? 'flag' : sim.stalledOut ? 'stall' : sim.timeLeft <= 0 ? 'timeout' : 'death';
        const stomps = ((sim.score - startScore) - (sim.won ? 1000 : 0)) / 100;
        console.log('  spawn col=' + String(col).padStart(3) +
                    '  decisions=' + String(decisions).padStart(3) +
                    '  maxCol=' + String(Math.floor(maxX/TILE)).padStart(3) +
                    '  stomps=' + stomps +
                    '  R=' + totalR.toFixed(2).padStart(6) +
                    '  ' + reason);
    }
    [2, 30, 50, 80, 95].forEach(rollout);

    console.log('\nStochastic rollouts (sample from policy, 5 each):');
    function sampledRollout(col) {
        let flags = 0;
        let bestX = 0;
        for (let trial = 0; trial < 5; trial++) {
            sim.setSpawn(col * TILE + 2, baseSpawnY - 4);
            sim.reset();
            let seed = (trial + 1) * 7919;
            function rng() { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 0x100000000; }
            for (let t = 0; t < 600; t++) {
                const obs = SwAgentObs.build(sim);
                xT.fromArray(obs);
                net.forward(xT, lgT);
                const probs = softmax(lgT.toArray());
                const a = sample(probs, rng);
                const out = sim.step(a);
                if (sim.player.x > bestX) bestX = sim.player.x;
                if (out.done) break;
            }
            if (sim.won) flags++;
        }
        console.log('  spawn col=' + String(col).padStart(3) +
                    '  flag rate=' + flags + '/5' +
                    '  bestX across trials=' + bestX.toFixed(0) +
                    ' (col ' + Math.floor(bestX/TILE) + ')');
    }
    [2, 30, 50, 80].forEach(sampledRollout);

    console.log('\n(headless inspector finished — exit any time)');
})();
