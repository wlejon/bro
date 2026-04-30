// trainer_worker.js — pure trainer.
//
// In:   tuples from main (sourced from mcts_worker × N), and
//       trajectory_end signals (used only for the trailing-mean checkpoint
//       metric and the last_n ring rotation).
// Out:  weights (broadcast to all mcts workers via main), stats for the HUD.
//
// Self-play and MCTS are NOT here. They live in `mcts_worker.js` (data
// generators at varying iter depths). This worker owns the network, the
// replay buffer, the SGD trainer, and the on-disk checkpoint ring + best.bin.
//
// Startup: load shared libs, build the trainer wiring (net + buffer +
// trainer + handle). Resume from `ckpt/best.bin` if present; otherwise
// run BC warmup against the heuristic + 5000 SGD pretrain. Then publish
// the initial weights and enter the ingest+train+publish loop.

'use strict';

self.Art = { drawTile() {} };
const fs = require('fs');

const SHARED = [
    '../lib/tilemap.js',
    '../lib/platformer.js',
    'level.js',
    'sim.js',
    'agent_obs.js',
    'agent.js',
    'bc_warmup.js',
];
for (const p of SHARED) {
    const src = fs.readFileSync(p, 'utf-8');
    (0, eval)(src);
}

const TILE = 32;

// ── Checkpoints ─────────────────────────────────────────────────────────────
const CKPT_DIR = 'apps/stompworld/ckpt';
const CKPT_RING_SIZE = 10;
const CKPT_BEST_WINDOW = 20;
let ckptRingIdx = 0;
let bestMean = -Infinity;
const recentReturns = [];

try { fs.mkdirSync(CKPT_DIR, { recursive: true }); } catch (_) {}

function safeWrite(path, bytes) {
    try { fs.writeFileSync(path, bytes); return true; }
    catch (e) { console.warn('checkpoint write failed:', path, e.message); return false; }
}
function safeWriteJson(path, obj) {
    try { fs.writeFileSync(path, JSON.stringify(obj, null, 2)); return true; }
    catch (e) { console.warn('checkpoint json write failed:', path, e.message); return false; }
}
function safeRead(path) {
    try { return fs.readFileSync(path); } catch (_) { return null; }
}
function safeReadText(path) {
    try { return fs.readFileSync(path, 'utf-8'); } catch (_) { return null; }
}

// ── Build agent (we use only net + buffer + trainer + handle) ───────────────
// Tilemap is built destructible so the new beam mechanic carves terrain
// during MCTS rollouts and BC episodes; without it, fire actions are
// silently no-ops on the trainer side.
function buildAgent() {
    const lvl = Level.buildLevel({ tileSize: TILE, destructible: true, trackDamagedTiles: false });
    const sim = SwSim.create({
        tilemap: lvl.tilemap,
        spawn: lvl.spawn,
        stompers: lvl.stompers, flyers: lvl.flyers,
        flag: lvl.flag, pickup: lvl.pickup,
        timeLimit: 20, stallDecisions: 50,
    });
    const agent = SwAgent.create({ sim });
    return { sim, agent, baseSpawnY: lvl.spawn.y };
}

const { sim, agent, baseSpawnY } = buildAgent();

// Publish initial (random or about-to-be-warmed-up) weights immediately so
// live + mcts workers can stand up their inference nets while we run BC
// warmup. We'll publish again after warmup with the trained weights.
{
    const bytes = agent.net.save();
    const buf = bytes.buffer;
    self.postMessage({
        type: 'weights', version: 0n, bytes,
        stats: { ingested: 0, bufSize: 0, trainSteps: 0,
                 lossValue: 0, lossPolicy: 0, netVersion: 0n,
                 bestMean: 0, meanReturn: 0, resumed: 0 },
    }, [buf]);
}

// ── Resume or warmup ────────────────────────────────────────────────────────
const bestMetaRaw = safeReadText(`${CKPT_DIR}/best.json`);
const bestBytes   = safeRead(`${CKPT_DIR}/best.bin`);
let warmupStats = null;
let resumedFromCheckpoint = false;

if (bestMetaRaw && bestBytes) {
    try {
        const meta = JSON.parse(bestMetaRaw);
        agent.net.load(new Uint8Array(bestBytes));
        bestMean = +meta.meanReturn || -Infinity;
        resumedFromCheckpoint = true;
        warmupStats = { resumed: true, meanReturn: bestMean, episode: meta.episode | 0 };
    } catch (e) {
        console.warn('checkpoint load failed:', e.message);
    }
}

if (!resumedFromCheckpoint) {
    const WARMUP_SPAWNS = [
        { col:  2, attempts: 40, minReward: -1.0 },
        { col: 12, attempts: 30, minReward: -1.0 },
        { col: 18, attempts: 30, minReward: -1.0 },
        { col: 32, attempts: 30, minReward: -1.0 },
        { col: 50, attempts: 30, minReward: -1.0 },
        { col: 70, attempts: 80, minReward:  0.2 },
    ];
    warmupStats = { attempts: 0, kept: 0, flags: 0, deaths: 0,
                    timeouts: 0, tuplesPushed: 0, avgEpisodeReward: 0 };
    for (const ws of WARMUP_SPAWNS) {
        const r = SwBcWarmup.populate(agent, sim, {
            targetSamples: ws.attempts,
            maxAttempts:   ws.attempts,
            gamma: 0.99,
            maxDecisions: 400,
            spawnX: ws.col * TILE + 2,
            spawnY: baseSpawnY - 4,
            minReward: ws.minReward,
            seed: 0xBC51A57E ^ (ws.col * 0x9E3779B1),
        });
        warmupStats.attempts     += r.attempts;
        warmupStats.kept         += r.kept;
        warmupStats.flags        += r.flags;
        warmupStats.deaths       += r.deaths;
        warmupStats.timeouts     += r.timeouts;
        warmupStats.tuplesPushed += r.tuplesPushed;
    }
    if (agent.buffer.size >= 32) {
        const last = agent.trainer.stepN(5000);
        warmupStats.pretrainSteps = 5000;
        warmupStats.pretrainLossPolicy = +last.lossPolicy || 0;
        warmupStats.pretrainLossValue  = +last.lossValue  || 0;
    } else {
        warmupStats.pretrainSteps = 0;
    }
}
self.postMessage({ type: 'warmup', stats: warmupStats });

// ── Tuple ingestion + train loop ────────────────────────────────────────────
// Single-head movement-only policy: policyTarget is a 6-element distribution
// (one entry per movement action). MCTS rootVisits and BC one-hots both
// arrive in this same shape, so no marginalization step is needed.
const PER_HEAD_TOTAL = SwSim.PER_HEAD_TOTAL;
const ACTION_MASK = new Float32Array(0);

let totalIngested = 0;
let totalTrainSteps = 0;
let lastLossValue = +(warmupStats && warmupStats.pretrainLossValue) || 0;
let lastLossPolicy = +(warmupStats && warmupStats.pretrainLossPolicy) || 0;
let lastVersionSent = -1n;

function ingestTuples(tuples, weight) {
    if (!tuples || !tuples.length) return 0;
    const repeats = Math.max(1, weight | 0);
    let n = 0;
    for (let k = 0; k < repeats; k++) {
        for (const t of tuples) {
            agent.buffer.push({
                obs: t.obs,
                policyTarget: t.policyTarget,
                actionMask: ACTION_MASK,
                valueTarget: +t.valueTarget || 0,
            });
            n++;
        }
    }
    return n;
}

// SGD cadence: each ingest message gets a small batch of SGD steps. Heavier
// ingests (more tuples) get proportionally more steps so a backlog doesn't
// rot in the buffer. Capped so a single huge message can't stall the worker.
const SGD_PER_TUPLE = 0.25;     // ~1 SGD per 4 tuples
const SGD_PER_INGEST_CAP = 200;

function trainAfterIngest(tuplesPushed) {
    if (agent.buffer.size < 32) return;
    const want = Math.min(SGD_PER_INGEST_CAP,
                          Math.max(4, Math.ceil(tuplesPushed * SGD_PER_TUPLE)));
    const last = agent.trainer.stepN(want);
    totalTrainSteps += want;
    lastLossValue  = +last.lossValue  || lastLossValue;
    lastLossPolicy = +last.lossPolicy || lastLossPolicy;
}

function snapshotStats() {
    const snap = agent.handle.snapshot();
    return {
        ingested:    totalIngested,
        bufSize:     agent.buffer.size | 0,
        trainSteps:  totalTrainSteps | 0,
        lossValue:   lastLossValue,
        lossPolicy:  lastLossPolicy,
        netVersion:  snap ? snap.version : 0n,
        bestMean:    Number.isFinite(bestMean) ? +bestMean : 0,
        meanReturn:  recentReturns.length > 0
                       ? recentReturns.reduce((a, b) => a + b, 0) / recentReturns.length
                       : 0,
        resumed:     resumedFromCheckpoint ? 1 : 0,
    };
}

function postWeightsAndStats() {
    const snap = agent.handle.snapshot();
    const v = snap ? snap.version : 0n;
    if (v === lastVersionSent) {
        self.postMessage({ type: 'stats', stats: snapshotStats() });
        return;
    }
    lastVersionSent = v;
    const bytes = agent.net.save();
    const buf = bytes.buffer;
    self.postMessage({
        type: 'weights', version: v, bytes,
        stats: snapshotStats(),
    }, [buf]);
}

// ── Trajectory end: trailing-mean return + checkpoint rotation ──────────────
function writeRingCheckpoint() {
    const bytes = agent.net.save();
    safeWrite(`${CKPT_DIR}/last_${ckptRingIdx}.bin`, bytes);
    ckptRingIdx = (ckptRingIdx + 1) % CKPT_RING_SIZE;
}

function maybeWriteBest(currentMean) {
    if (currentMean > bestMean) {
        bestMean = currentMean;
        const bytes = agent.net.save();
        safeWrite(`${CKPT_DIR}/best.bin`, bytes);
        const snap = agent.handle.snapshot();
        safeWriteJson(`${CKPT_DIR}/best.json`, {
            meanReturn: currentMean,
            window: CKPT_BEST_WINDOW,
            netVersion: snap ? snap.version.toString() : '0',
            ingested: totalIngested,
            trainSteps: totalTrainSteps,
        });
        self.postMessage({ type: 'best', meanReturn: currentMean });
    }
}

function onTrajectoryEnd(totalReturn /*, reason */) {
    recentReturns.push(+totalReturn || 0);
    if (recentReturns.length > CKPT_BEST_WINDOW) recentReturns.shift();
    writeRingCheckpoint();
    if (recentReturns.length >= CKPT_BEST_WINDOW) {
        let s = 0;
        for (const r of recentReturns) s += r;
        maybeWriteBest(s / recentReturns.length);
    }
}

// ── Message dispatch ────────────────────────────────────────────────────────
self.onmessage = (e) => {
    const m = e && e.data; if (!m) return;
    if (m.type === 'tuples') {
        const n = ingestTuples(m.tuples, m.weight);
        totalIngested += n;
        trainAfterIngest(n);
        postWeightsAndStats();
    } else if (m.type === 'trajectory_end') {
        onTrajectoryEnd(m.totalReturn, m.reason);
    } else if (m.type === 'stop') {
        // Drain — nothing to do, the engine tears us down.
    }
};

// Initial publish so live + mcts workers can stand up their inference nets.
postWeightsAndStats();
