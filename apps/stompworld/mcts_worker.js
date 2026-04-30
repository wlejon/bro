// mcts_worker.js — pure self-play data generator.
//
// One worker = one configured search depth. The Training screen spawns
// N of these with different `iterations` / `rolloutDepth` so the trainer
// gets data sampled across both shallow (cheap, more episodes/sec) and
// deep (expensive, higher-quality visits) search distributions. The
// network learns more from a mix than from a single setting, and at
// inference time the live worker still gets to run *additional* MCTS on
// top of any of these workers' best trajectories via the best-crop pool.
//
// Behavior knobs that distinguish this from the live worker:
//   - Dirichlet noise on the root prior is ON (broad exploration).
//   - History tapes (failure + success) ARE active here too — every
//     terminal's tail suppresses identical (sig, action) replays in
//     future searches, regardless of outcome. This is the main lever
//     for diverse training data: forces every success to be a *new*
//     trajectory and every death to be a *new* failure.
//   - No render — we don't post snapshots, just tuples + trajectories.
//   - Spawn columns are sampled uniformly random (mirrors the curriculum
//     the old monolithic trainer ran).
//
// Per-episode payload to main:
//   { type: 'tuples',     source: 'mcts', workerId, reason, tuples, weight }
//   { type: 'trajectory', source: 'mcts', workerId, startSnap, actions,
//                         decisions, totalReturn, bestX, searchDepth, reason }

'use strict';

self.Art = { drawTile() {} };
const fs = require('fs');

const SHARED = [
    '../lib/tilemap.js',
    '../lib/platformer.js',
    'level.js',
    'sim.js',
    'agent_obs.js',
    'play_agent.js',
];
for (const p of SHARED) {
    const src = fs.readFileSync(p, 'utf-8');
    (0, eval)(src);
}

const TILE = 32;
const SPAWN_COL = 2;
const TRAIN_TIME_LIMIT = 30;

function buildSim() {
    const lvl = Level.buildLevel({ tileSize: TILE, destructible: true, trackDamagedTiles: false });
    const sim = SwSim.create({
        tilemap: lvl.tilemap,
        spawn: lvl.spawn,
        stompers: lvl.stompers, flyers: lvl.flyers,
        flag: lvl.flag, pickup: lvl.pickup,
        timeLimit: TRAIN_TIME_LIMIT,
        stallDecisions: 50,
    });
    return { sim, baseSpawnY: lvl.spawn.y };
}

// Coarse state signature shared with the history tapes. Same shape as
// the prior live worker's: (col, row, onGround, vxSign, hasWeapon).
function buildSig(s) {
    const p = s.player;
    const col = Math.floor(p.x / TILE);
    const row = Math.floor(p.y / TILE);
    const og  = p.onGround ? 1 : 0;
    const vxSign = p.vx > 8 ? 1 : (p.vx < -8 ? -1 : 0);
    const hw = s.hasWeapon ? 1 : 0;
    return col + ',' + row + ',' + og + ',' + vxSign + ',' + hw;
}

let workerId = 0;
let agent = null;
let sim = null;
let baseSpawnY = 0;
let running = false;
let inEpisode = false;
let episodes = 0;
let lastReason = 'fresh';
let decisionsSinceStats = 0;
const STATS_EVERY_DECISIONS = 200;
const DECISIONS_PER_TICK = 32;

// History tape — single shared mask across ALL workers. Failure-like
// terminals (death / stall / timeout) push the (sig, action) tail onto
// the tape so the next MCTS search at the same coarse state down-weights
// the recorded action. Flag-successes do NOT go on the tape: masking
// wins drove the agent away from winning lines and left stalls as the
// best-return trajectory in the visualizer. Workers post each recorded
// trace to main as 'tape_record'; main fans it out to every other worker
// as 'tape_apply' so a deep worker's failure pattern constrains shallow
// workers' priors and vice versa. The local recordFailure call on
// episode end is the source's own copy (no self-broadcast).
let historyTape = null;
const sigList = [];

function pickSpawn() {
    sim.setSpawn(SPAWN_COL * TILE + 2, baseSpawnY - 4);
}

function startEpisode() {
    pickSpawn();
    sim.reset();
    agent.startEpisode();
    sigList.length = 0;
    inEpisode = true;
}

function endEpisode(reason) {
    const r = agent.endEpisode(reason);
    // Only failure-like outcomes go on the tape. Masking flag-successes
    // pushed the agent away from winning lines, leaving stalls as the
    // best-return trajectory in the visualizer's ring. The NN training
    // path still reinforces wins via supervised tuples; the tape's job
    // is purely to nudge MCTS away from re-trying losing tails.
    const isFailure = reason === 'death' || reason === 'stall' || reason === 'timeout';
    const len = Math.min(sigList.length, r.actions.length);
    if (isFailure && len > 0) {
        const trace = new Array(len);
        for (let i = 0; i < len; i++) {
            trace[i] = { sig: sigList[i], action: r.actions[i] };
        }
        historyTape.recordFailure(trace);
        try {
            self.postMessage({
                type: 'tape_record', source: 'mcts', workerId, trace,
            });
        } catch (_) {}
    }
    self.postMessage({
        type: 'tuples', source: 'mcts', workerId, reason,
        tuples: r.tuples,
        weight: reason === 'flag' ? 3 : 1,
    });
    self.postMessage({
        type: 'trajectory', source: 'mcts', workerId,
        startSnap: r.startSnap, actions: r.actions,
        decisions: r.decisions, totalReturn: r.totalReturn,
        bestX: r.bestX, searchDepth: agent.iterations,
        reason,
    });
    episodes++;
    lastReason = reason;
    inEpisode = false;
}

// Message-driven: main pumps 'tick' messages to keep the worker fed.
// Worker timer scheduling beyond trivial micro-delays is unreliable in
// this runtime, so the only stable pattern is: do work in onmessage,
// post results, wait for the next message. Main can pump fast — each
// tick processes a batch of decisions before yielding.
function runBatch() {
    if (!running || !agent) return;
    if (!agent.weightsLoaded) {
        // Stay in the pump but don't burn cycles; main will keep ticking.
        self.postMessage({ type: 'ready', source: 'mcts', workerId });
        return;
    }
    for (let i = 0; i < DECISIONS_PER_TICK; i++) {
        if (!inEpisode) startEpisode();
        sigList.push(buildSig(sim));
        const action = agent.decide();
        const out = agent.applyAction(action);
        if (out.done) {
            const reason = sim.won           ? 'flag'
                         : sim.stalledOut    ? 'stall'
                         : sim.timeLeft <= 0 ? 'timeout'
                         :                     'death';
            endEpisode(reason);
        }
        decisionsSinceStats++;
        if (decisionsSinceStats >= STATS_EVERY_DECISIONS) {
            self.postMessage({
                type: 'stats', source: 'mcts', workerId,
                episodes, lastReason,
                iterations: agent.iterations,
                netVersion: agent.netVersion,
                tapeSize:     historyTape ? historyTape.size     : 0,
                tapeCapacity: historyTape ? historyTape.capacity : 0,
            });
            decisionsSinceStats = 0;
        }
    }
    // Tell main we're ready for another batch. Main treats this as a
    // self-clocking signal — keeps the worker fed without flooding the
    // queue when it's busy.
    self.postMessage({ type: 'ready', source: 'mcts', workerId });
}

self.onmessage = (e) => {
    const m = e && e.data; if (!m) return;
    if (m.type === 'init') {
        workerId = m.workerId | 0;
        const built = buildSim();
        sim = built.sim; baseSpawnY = built.baseSpawnY;
        // Single tape per worker. Main keeps every worker's tape in sync
        // by broadcasting each finished trace to peers — the local store
        // here mirrors that union. tapeDepth is high so a long episode is
        // recorded in full (the C++ tape truncates to the last tapeDepth
        // entries internally); ringCapacity caps the live (sig, action)
        // entries across all recorded traces.
        historyTape = bro.ai.game.grid.createFailureTape({
            tapeDepth:    1024,
            ringCapacity: 500,
            penalty:      0.1,
            floor:        0.001,
        });
        agent = PlayAgent.create({
            sim,
            iterations:   m.iterations   | 0 || 100,
            rolloutDepth: m.rolloutDepth | 0 || 8,
            dirichletAlpha:   m.dirichletAlpha   != null ? m.dirichletAlpha   : 0.5,
            dirichletEpsilon: m.dirichletEpsilon != null ? m.dirichletEpsilon : 0.25,
            seed: BigInt(m.workerId | 0) * 0x9E3779B1n ^ 0xA11CE5n,
            sigFn: () => buildSig(sim),
            priorAdjust: (sig, prior) => historyTape.applyPriors(sig, prior),
        });
        if (m.bytes) agent.setWeights(new Uint8Array(m.bytes), m.version);
        running = true;
        // Signal readiness; main will start pumping ticks once it sees this.
        self.postMessage({ type: 'ready', source: 'mcts', workerId });
    } else if (m.type === 'weights') {
        if (agent) agent.setWeights(new Uint8Array(m.bytes), m.version);
    } else if (m.type === 'tape_apply') {
        // A peer worker just finished an episode; main forwarded the
        // trace here so our priors mirror the union.
        if (historyTape && m.trace && m.trace.length) {
            historyTape.recordFailure(m.trace);
        }
    } else if (m.type === 'clear_tape') {
        if (historyTape) historyTape.clear();
    } else if (m.type === 'tick') {
        runBatch();
    } else if (m.type === 'stop') {
        running = false;
    }
};
