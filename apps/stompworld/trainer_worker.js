// trainer_worker.js — background self-play + ExIt training for stompworld.
//
// Runs the full expert (high-iter MCTS) + apprentice (PolicyValueNet) loop
// on its own OS thread. After each episode the trainer fires SGD steps;
// fresh weights are serialized via PolicyValueNet.save() and shipped to
// the main thread, which feeds them into the live agent driving the
// displayed game.
//
// Shared libs aren't reachable via <script src> in a worker (single-file
// execution), so we synchronously fs.readFileSync + (0,eval) each one.
// Their `(typeof window !== 'undefined' ? window : globalThis)` IIFE
// pattern attaches to globalThis here unchanged.

'use strict';

// ── Load shared libs ────────────────────────────────────────────────────────
// Stub Art before loading level.js — Tilemap only invokes drawTile during
// rendering, which we never do in the worker.
self.Art = { drawTile() {} };

const fs = require('fs');

const SHARED = [
    '../lib/tilemap.js',
    '../lib/platformer.js',
    '../lib/mcts_js.js',
    'level.js',
    'sim.js',
    'agent_obs.js',
    'agent.js',
];
for (const p of SHARED) {
    const src = fs.readFileSync(p, 'utf-8');
    (0, eval)(src);
}

const TILE = 32;

// ── Build sim + agent ───────────────────────────────────────────────────────
function buildAgent() {
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
    const sim = SwSim.create({
        tilemap: lvl.tilemap,
        spawn, stompers: stomperTemplates, flag,
        timeLimit: 300,
    });
    const agent = SwAgent.create({ sim });
    return { sim, agent };
}

const { sim, agent } = buildAgent();

// ── Weights export ──────────────────────────────────────────────────────────
// PolicyValueNet.save() returns a Uint8Array backed by a fresh ArrayBuffer
// that we hand off via the transfer list — zero-copy across threads.
let lastVersionSent = -1n;

function postWeightsAndStats() {
    const s = agent.stats;
    const v = BigInt(s.netVersion || 0n);
    if (v === lastVersionSent) {
        // No new publish since last send — just push stats so the HUD updates.
        self.postMessage({ type: 'stats', stats: snapshotStats(s) });
        return;
    }
    lastVersionSent = v;
    const bytes = agent.net.save();
    const buf = bytes.buffer;
    self.postMessage({
        type: 'weights',
        version: v,
        bytes,
        stats: snapshotStats(s),
    }, [buf]);
}

function snapshotStats(s) {
    // Copy primitives only — BigInts survive structured clone.
    return {
        episode:      s.episode | 0,
        iters:        s.iters | 0,
        steps:        s.steps | 0,
        bestX:        s.bestX | 0,
        lastReason:   String(s.lastReason || ''),
        lossValue:    +s.lossValue || 0,
        lossPolicy:   +s.lossPolicy || 0,
        trainSteps:   s.trainSteps | 0,
        netVersion:   BigInt(s.netVersion || 0n),
        bufSize:      s.bufSize | 0,
        tuplesPushed: s.tuplesPushed | 0,
    };
}

// ── Main loop ───────────────────────────────────────────────────────────────
// We run a fixed batch of decisions per tick, then yield via setTimeout(0)
// so the worker stays responsive to onmessage (e.g. a stop request) and
// doesn't starve other JS jobs (timers, fetch).
let running = true;
const DECISIONS_PER_TICK = 64;
const STATS_EVERY_DECISIONS = 200;
let decisionsSinceStats = 0;

function tick() {
    if (!running) return;
    for (let i = 0; i < DECISIONS_PER_TICK; i++) {
        const action = agent.decide();
        const out = agent.applyAction(action);
        agent.recordFrameTick();
        if (out.done) {
            const reason = sim.won ? 'flag'
                         : (sim.timeLeft <= 0 ? 'timeout' : 'death');
            agent.endEpisode(reason, /*ghosts*/ null);
            agent.resetEpisode();
            postWeightsAndStats();
            decisionsSinceStats = 0;
        } else {
            decisionsSinceStats++;
            if (decisionsSinceStats >= STATS_EVERY_DECISIONS) {
                self.postMessage({ type: 'stats', stats: snapshotStats(agent.stats) });
                decisionsSinceStats = 0;
            }
        }
    }
    setTimeout(tick, 0);
}

self.onmessage = (e) => {
    const msg = e && e.data;
    if (!msg) return;
    if (msg.type === 'stop') {
        running = false;
        // Optional: bro.server.stop() would terminate the worker outright.
    }
};

// Send the initial (untrained) weights so the main thread can stand up its
// inference net immediately — otherwise the live agent has nothing to run.
postWeightsAndStats();
setTimeout(tick, 0);
