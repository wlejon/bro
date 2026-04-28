// live_worker.js — owns the displayed sim and runs MCTS on top of the
// best-crop substrate that the main thread feeds it.
//
// One worker, one sim. Per decision:
//   1. Build a coarse state signature for the failure tape.
//   2. MCTS-decide using the current network weights, with the failure
//      tape biasing the prior away from recent fatal actions at this sig.
//   3. Step the sim, append a (obs, visits, reward) tuple, post a render
//      snapshot to main.
// On terminal:
//   - Post tuples + trajectory metadata to main (which forwards tuples
//     to the trainer worker and ingests the trajectory into the pool).
//   - If the terminal was a death/stall/timeout, splice the last K
//     decisions into the failure tape so we don't repeat the line.
//   - Apply the next pending seed (if any) and start a fresh episode.
//
// Pacing: a wall-clock target period (~67 ms per decision = the same
// FRAME_SKIP cadence the sim uses) divided by `fastMult`. The main
// thread bumps fastMult on F-key. The display draws at 60 fps from the
// most recent snapshot regardless of how fast the worker runs.

'use strict';

self.Art = { drawTile() {} };
const fs = require('fs');

const SHARED = [
    '../lib/tilemap.js',
    '../lib/platformer.js',
    '../lib/mcts_js.js',
    'level.js',
    'sim.js',
    'agent_obs.js',
    'failure_tape.js',
    'play_agent.js',
];
for (const p of SHARED) {
    const src = fs.readFileSync(p, 'utf-8');
    (0, eval)(src);
}

const TILE = 32;

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
        bobFreq: bob ? Math.PI : 0,
        bobT: 0, animT: 0,
    };
}

function buildSim() {
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
            flyerTemplates.push(makeFlyer(e));
        } else if (e.kind === 'flag') {
            flag = { x: e.x, w: 32, h: 96, y: e.row * TILE - 64 };
            flag.y = e.row * TILE - flag.h + TILE;
        }
    }
    const sim = SwSim.create({
        tilemap: lvl.tilemap,
        spawn, stompers: stomperTemplates, flyers: flyerTemplates, flag,
        timeLimit: 300,
        stallDecisions: 45, stallEpsilonPx: 8,
    });
    return { sim, baseSpawnY: spawn.y, defaultSpawnX: spawn.x };
}

const { sim, baseSpawnY, defaultSpawnX } = buildSim();
const tape = FailureTape.create({
    maxEntries: 200, lookback: 8, penalty: 0.1,
    numActions: sim.numActions, tile: TILE,
});

// Live MCTS depth is sized to fit comfortably inside one decision period
// (LIVE_PERIOD_MS = 67 ms in main = sim FRAME_SKIP). A trained network
// already has good policy outputs, so a shallow on-top search plus the
// failure-tape bias is enough to refine. The deep searches that produce
// new training data live in the mcts workers, not here.
const agent = PlayAgent.create({
    sim,
    iterations: 24,
    rolloutDepth: 4,
    dirichletAlpha: 0.0,
    dirichletEpsilon: 0.0,
    sigFn: () => tape.buildSig(sim),
    priorBias: (sig, legal) => tape.biasFor(sig, legal),
});

let pendingSeed = null;     // {startSnap, prefixActions?} or {spawnCol}
let running = true;
let inEpisode = false;
const sigList = [];
let episodes = 0;
let lastReason = 'fresh';
let bestXEpisode = 0;
let decisionsTotal = 0;

function applySeed() {
    if (!pendingSeed) {
        sim.setSpawn(defaultSpawnX, baseSpawnY - 4);
        sim.reset();
        return null;
    }
    const seed = pendingSeed; pendingSeed = null;
    if (seed.startSnap) {
        // Restore baseline state (keeps array refs stable), then overlay
        // the snapshot. Replay any prefix actions deterministically so
        // the live agent picks up at the interesting tail of the chosen
        // trajectory rather than from spawn.
        sim.setSpawn(defaultSpawnX, baseSpawnY - 4);
        sim.reset();
        try {
            sim.restore(seed.startSnap);
        } catch (e) {
            // If a snapshot fails to restore (shape mismatch from a
            // weights-era change, etc.), fall back cleanly to spawn.
            sim.reset();
            return null;
        }
        if (seed.prefixActions && seed.prefixActions.length) {
            for (const a of seed.prefixActions) {
                const out = sim.step(a);
                if (out.done) break;
            }
        }
        return seed;
    }
    if (seed.spawnCol != null) {
        sim.setSpawn(seed.spawnCol * TILE + 2, baseSpawnY - 4);
        sim.reset();
        return seed;
    }
    sim.setSpawn(defaultSpawnX, baseSpawnY - 4);
    sim.reset();
    return seed;
}

function makeSnap(extra) {
    const p = sim.player;
    const stompers = [];
    for (const s of sim.stompers) {
        stompers.push({
            x: s.x, y: s.y, w: s.w, h: s.h,
            alive: !!s.alive, animT: s.animT, vx: s.vx,
            squashTimer: s.squashTimer,
        });
    }
    const flyers = [];
    for (const f of sim.flyers) {
        flyers.push({
            x: f.x, y: f.y, w: f.w, h: f.h,
            vx: f.vx, animT: f.animT,
        });
    }
    return Object.assign({
        tick: sim.tick,
        player: {
            x: p.x, y: p.y, vx: p.vx, vy: p.vy,
            onGround: !!p.onGround, facing: p.facing,
            w: p.w, h: p.h,
        },
        stompers, flyers,
        episodes, lastReason,
        bestX: bestXEpisode,
        decisions: decisionsTotal,
        tapeEntries: tape.entries(),
        tapeSigs: tape.sigs(),
        netVersion: agent.netVersion,
        timeLeft: sim.timeLeft,
    }, extra || {});
}

function startEpisode() {
    applySeed();
    agent.startEpisode();
    sigList.length = 0;
    bestXEpisode = sim.player.x;
    inEpisode = true;
    self.postMessage({ type: 'render', snap: makeSnap({ event: 'start' }) });
}

function endEpisode(reason) {
    const result = agent.endEpisode(reason);
    if (reason === 'death' || reason === 'stall' || reason === 'timeout') {
        tape.recordFailure(sigList, result.actions);
    }
    self.postMessage({
        type: 'tuples',
        source: 'live',
        reason,
        tuples: result.tuples,
        weight: reason === 'flag' ? 3 : 1,
    });
    self.postMessage({
        type: 'trajectory',
        source: 'live',
        startSnap: result.startSnap,
        actions: result.actions,
        decisions: result.decisions,
        totalReturn: result.totalReturn,
        bestX: result.bestX,
        searchDepth: agent.iterations,
        reason,
    });
    episodes++;
    lastReason = reason;
    inEpisode = false;
}

// Worker timer scheduling beyond trivial micro-delays is unreliable in
// this runtime. The live worker self-clocks via a 'ready' / 'step' loop
// with main: after each decision (or stall on no-weights) we post
// 'ready'. Main answers with 'step' when wall-clock pacing says it's
// time. This gives perfect back-pressure — the worker never queues up
// more decisions than main has asked for, no matter how slow MCTS gets.
function doStep() {
    if (!running) return;
    if (!agent.weightsLoaded) {
        self.postMessage({ type: 'ready', source: 'live' });
        return;
    }
    if (!inEpisode) startEpisode();
    sigList.push(tape.buildSig(sim));
    const action = agent.decide();
    const out = agent.applyAction(action);
    decisionsTotal++;
    if (sim.player.x > bestXEpisode) bestXEpisode = sim.player.x;
    self.postMessage({ type: 'render', snap: makeSnap() });
    if (out.done) {
        const reason = sim.won            ? 'flag'
                     : sim.stalledOut     ? 'stall'
                     : sim.timeLeft <= 0  ? 'timeout'
                     :                      'death';
        endEpisode(reason);
    }
    self.postMessage({ type: 'ready', source: 'live' });
}

self.onmessage = (e) => {
    const m = e && e.data; if (!m) return;
    if (m.type === 'weights') {
        const wasLoaded = agent.weightsLoaded;
        agent.setWeights(new Uint8Array(m.bytes), m.version);
        // First weights ever — re-arm the loop in case main was waiting
        // for us to finish stalling.
        if (!wasLoaded) self.postMessage({ type: 'ready', source: 'live' });
    } else if (m.type === 'step') {
        doStep();
    } else if (m.type === 'seed') {
        pendingSeed = m;
    } else if (m.type === 'clear_failures') {
        tape.clear();
    } else if (m.type === 'stop') {
        running = false;
    }
};

// Initial readiness signal so main starts pumping immediately. We may
// not have weights yet — if so doStep above will simply re-post ready.
self.postMessage({ type: 'ready', source: 'live' });
