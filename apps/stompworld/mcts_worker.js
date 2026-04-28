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
//   - No failure tape (we WANT to revisit failed lines from different
//     network states; that's how the data stays diverse).
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
// Includes col 100 (near the pickup at col 115) so a fraction of episodes
// can capture full pickup → backtrack-and-destroy → flag arcs within the
// training horizon. The lower spawns still feed local-skill data (jumping,
// stomping, beam against terrain).
const SPAWN_COLS = [2, 32, 48, 78, 100];
const TRAIN_TIME_LIMIT = 30;

function buildSim() {
    const lvl = Level.buildLevel({ tileSize: TILE, destructible: true });
    const sim = SwSim.create({
        tilemap: lvl.tilemap,
        spawn: lvl.spawn,
        stompers: lvl.stompers, flyers: lvl.flyers,
        flag: lvl.flag, pickup: lvl.pickup,
        timeLimit: TRAIN_TIME_LIMIT,
        stallDecisions: 90,
    });
    return { sim, baseSpawnY: lvl.spawn.y };
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

function pickSpawn() {
    const idx = Math.floor(Math.random() * SPAWN_COLS.length);
    sim.setSpawn(SPAWN_COLS[idx] * TILE + 2, baseSpawnY - 4);
}

function startEpisode() {
    pickSpawn();
    sim.reset();
    agent.startEpisode();
    inEpisode = true;
}

function endEpisode(reason) {
    const r = agent.endEpisode(reason);
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
        agent = PlayAgent.create({
            sim,
            iterations:   m.iterations   | 0 || 100,
            rolloutDepth: m.rolloutDepth | 0 || 8,
            dirichletAlpha:   m.dirichletAlpha   != null ? m.dirichletAlpha   : 0.5,
            dirichletEpsilon: m.dirichletEpsilon != null ? m.dirichletEpsilon : 0.25,
            seed: BigInt(m.workerId | 0) * 0x9E3779B1n ^ 0xA11CE5n,
        });
        if (m.bytes) agent.setWeights(new Uint8Array(m.bytes), m.version);
        running = true;
        // Signal readiness; main will start pumping ticks once it sees this.
        self.postMessage({ type: 'ready', source: 'mcts', workerId });
    } else if (m.type === 'weights') {
        if (agent) agent.setWeights(new Uint8Array(m.bytes), m.version);
    } else if (m.type === 'tick') {
        runBatch();
    } else if (m.type === 'stop') {
        running = false;
    }
};
