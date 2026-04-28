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
    'bc_warmup.js',
];
for (const p of SHARED) {
    const src = fs.readFileSync(p, 'utf-8');
    (0, eval)(src);
}

const TILE = 32;

// ── Checkpoints ─────────────────────────────────────────────────────────────
// Two ring buffers persisted to disk:
//   ckpt/last_<n>.bin  — last 10 published-weights snapshots, indexed by
//                        a wrapping counter (so re-runs overwrite oldest).
//   ckpt/best.bin      — single best snapshot ever observed, by 20-episode
//                        trailing mean return. Also written: best.json with
//                        the metric value and episode it was sealed at.
// On startup we look for ckpt/best.bin and load it before warmup so we
// resume from prior progress instead of re-pretraining from scratch. If
// any disk operation fails we just log and continue — the worker is the
// owner of the run; nothing else depends on these files.
// Worker's CWD is the bro root (it's the binary's process CWD), but
// readFileSync('level.js') works because brokit's resolver checks
// registered base paths first. mkdirSync/writeFileSync don't get that
// fallback (they don't fs::exists-probe), so we use an explicit path
// rooted at the app dir relative to bro root.
const CKPT_DIR = 'apps/stompworld/ckpt';
const CKPT_RING_SIZE = 10;
const CKPT_BEST_WINDOW = 20;
let ckptRingIdx = 0;
let bestMean = -Infinity;
const recentReturns = [];   // trailing window of total episode rewards

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

// ── Build sim + agent ───────────────────────────────────────────────────────
// Curriculum: train from progressively harder spawn columns. We advance to
// the next column once the agent flags 3 of its last 10 episodes from the
// current spawn, then loop back to col 2 after the final one is solved
// (keeps the easy-case freshness in the buffer). The shorter timeLimit
// (~20s ≈ 300 decisions) aligns the discounted-return horizon with γ=0.99.
// Curriculum spawns. Avoid columns that contain (or sit adjacent to) a
// stomper — stompers live at cols 17, 28, 42, 53, 58, 62, 80, 90, and a
// fresh-spawn AABB at one of those cols starts inside the stomper and
// dies in 1 decision. Col 78 is the BC warmup col (long flat run to flag,
// guaranteed clear) and works as a "near-flag" rung.
const SPAWN_COLS = [2, 32, 48, 78];
const TRAIN_TIME_LIMIT = 20;

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
        bobT:    0,
        animT:   0,
    };
}

function buildAgent() {
    const lvl = Level.load({ tileSize: TILE });
    let spawn = { x: 0, y: 0 };
    const stomperTemplates = [];
    const flyerTemplates = [];
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
        timeLimit: TRAIN_TIME_LIMIT,
        stallDecisions: 30,
    });
    const agent = SwAgent.create({ sim });
    // The level's player spawn is always row 15. We use it as the row to
    // sit at when re-spawning at later columns (the level guarantees ground
    // is present at every checkpoint column).
    const baseSpawnY = spawn.y;
    return { sim, agent, baseSpawnY };
}

const { sim, agent, baseSpawnY } = buildAgent();

// Try to resume from the best previous checkpoint. If we find one, we
// skip BC warmup entirely — the saved net already encodes (or surpasses)
// what the heuristic could teach.
const bestMetaRaw = safeRead(`${CKPT_DIR}/best.json`);
const bestBytes   = safeRead(`${CKPT_DIR}/best.bin`);
let warmupStats = null;
let resumedFromCheckpoint = false;

if (bestMetaRaw && bestBytes) {
    try {
        const meta = JSON.parse(bestMetaRaw.toString('utf-8'));
        agent.net.load(new Uint8Array(bestBytes));
        bestMean = +meta.meanReturn || -Infinity;
        resumedFromCheckpoint = true;
        warmupStats = { resumed: true, meanReturn: bestMean, episode: meta.episode | 0 };
    } catch (e) {
        console.warn('checkpoint load failed:', e.message);
    }
}

if (!resumedFromCheckpoint) {
    // Multi-spawn BC warmup. Single-col demos (col 70 only) gave the net
    // narrow coverage and the policy generalized "JR everywhere" because
    // it never saw early-level states. Now we run the heuristic from a
    // grid of spawn cols and keep ALL non-trivial trajectories — even
    // partial-progress death runs teach the policy "walk right" at the
    // states they cover. Per-col target counts are biased toward cols
    // where the heuristic actually flags, but every col contributes some
    // tuples regardless.
    const WARMUP_SPAWNS = [
        // col 70 is the only spot where the heuristic reliably flags,
        // so we weight it heaviest; the others contribute partial-progress
        // demos to teach early-level "walk right under flyer / jump pit"
        // behaviors. minReward=-1 = keep every attempted trajectory.
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
            targetSamples: ws.attempts,    // keep up to all attempts
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
    // Pre-train hard on the demo buffer so the first weight publish actually
    // reflects the heuristic. 5000 SGD × batch 32 ≈ 160k tuple visits — the
    // demo set should be solidly fitted by the time we go online.
    if (agent.buffer.size >= 32) {
        const last = agent.trainer.stepN(5000);
        warmupStats.pretrainSteps   = 5000;
        warmupStats.pretrainLossPolicy = +last.lossPolicy || 0;
        warmupStats.pretrainLossValue  = +last.lossValue  || 0;
    } else {
        warmupStats.pretrainSteps = 0;
    }
}
self.postMessage({ type: 'warmup', stats: warmupStats });

// ── Curriculum: uniform random sampling over all spawn cols ─────────────────
// Sequential curriculum had a "forget the easy stuff" problem: once advanced
// past col 2, the agent never re-experienced gap-1 / first-stomper states,
// and the net's confident "JR everywhere" generalization went unchecked.
// Uniform sampling keeps every spawn fresh in the FIFO buffer at all times.
// Per-col flag rate is tracked just for HUD reporting.
let spawnIdx = 0;                         // current episode's spawn index
const perColAttempts = new Array(SPAWN_COLS.length).fill(0);
const perColFlags    = new Array(SPAWN_COLS.length).fill(0);

function pickAndApplySpawn() {
    spawnIdx = Math.floor(Math.random() * SPAWN_COLS.length);
    const col = SPAWN_COLS[spawnIdx];
    sim.setSpawn(col * TILE + 2, baseSpawnY - 4);
}
pickAndApplySpawn();
sim.reset();

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
        spawnCol:     SPAWN_COLS[spawnIdx] | 0,
        spawnIdx:     spawnIdx | 0,
        bestMean:     Number.isFinite(bestMean) ? +bestMean : 0,
        meanReturn:   recentReturns.length > 0
                        ? recentReturns.reduce((a, b) => a + b, 0) / recentReturns.length
                        : 0,
        resumed:      resumedFromCheckpoint ? 1 : 0,
        // Per-col flag rate as "f/a" string for compact HUD display.
        perColRates:  SPAWN_COLS.map((c, i) =>
                        c + ':' + perColFlags[i] + '/' + perColAttempts[i]).join(' '),
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

let episodeReturn = 0;

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
        safeWriteJson(`${CKPT_DIR}/best.json`, {
            meanReturn: currentMean,
            window: CKPT_BEST_WINDOW,
            episode: agent.stats.episode | 0,
            netVersion: agent.stats.netVersion ? agent.stats.netVersion.toString() : '0',
            spawnCol: SPAWN_COLS[spawnIdx],
        });
        self.postMessage({ type: 'best', meanReturn: currentMean,
                           episode: agent.stats.episode | 0 });
    }
}

function tick() {
    if (!running) return;
    for (let i = 0; i < DECISIONS_PER_TICK; i++) {
        const action = agent.decide();
        const out = agent.applyAction(action);
        episodeReturn += out.reward;
        agent.recordFrameTick();
        if (out.done) {
            const reason = sim.won           ? 'flag'
                         : sim.stalledOut    ? 'stall'
                         : sim.timeLeft <= 0 ? 'timeout'
                         :                     'death';
            agent.endEpisode(reason, /*ghosts*/ null);

            // Trailing-mean return for "best" checkpoint selection.
            recentReturns.push(episodeReturn);
            if (recentReturns.length > CKPT_BEST_WINDOW) recentReturns.shift();
            episodeReturn = 0;

            // Per-spawn-col flag tracking (for HUD only — sampling is
            // uniform random regardless of past performance).
            perColAttempts[spawnIdx]++;
            if (reason === 'flag') perColFlags[spawnIdx]++;

            // Always rotate into the last-N ring; conditionally seal "best".
            writeRingCheckpoint();
            if (recentReturns.length >= CKPT_BEST_WINDOW) {
                let s = 0;
                for (const r of recentReturns) s += r;
                maybeWriteBest(s / recentReturns.length);
            }

            pickAndApplySpawn();
            agent.resetEpisode();      // applies the new spawn via sim.reset()
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

// Action mask for live tuples: all 6 actions are always legal in this sim
// (jumps from the air no-op rather than being filtered out), matching what
// SwAgent.endEpisode pushes for self-play.
const LIVE_ACTION_MASK = new Float32Array(agent.sim.numActions);
for (let i = 0; i < LIVE_ACTION_MASK.length; i++) LIVE_ACTION_MASK[i] = 1;

const LIVE_FLAG_PUSH_MULT = 3;

function ingestLiveTuples(tuples, reason) {
    if (!tuples || tuples.length === 0) return 0;
    const repeats = (reason === 'flag') ? LIVE_FLAG_PUSH_MULT : 1;
    let pushed = 0;
    for (let k = 0; k < repeats; k++) {
        for (const t of tuples) {
            agent.buffer.push({
                obs: t.obs,
                policyTarget: t.policyTarget,
                actionMask: LIVE_ACTION_MASK,
                valueTarget: +t.valueTarget || 0,
            });
            pushed++;
        }
    }
    return pushed;
}

self.onmessage = (e) => {
    const msg = e && e.data;
    if (!msg) return;
    if (msg.type === 'stop') {
        running = false;
        // Optional: bro.server.stop() would terminate the worker outright.
    } else if (msg.type === 'live_tuples') {
        ingestLiveTuples(msg.tuples, msg.reason);
    }
};

// Send the initial (untrained) weights so the main thread can stand up its
// inference net immediately — otherwise the live agent has nothing to run.
postWeightsAndStats();
setTimeout(tick, 0);
