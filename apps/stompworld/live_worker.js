// live_worker.js — owns the displayed sim and runs MCTS on top of the
// best-crop substrate that the main thread feeds it.
//
// One worker, one sim. Per decision:
//   1. Build a coarse state signature for the history tapes.
//   2. MCTS-decide using the current network weights, with both history
//      tapes (failure + success) biasing the prior away from previously-
//      walked (sig, action) pairs so search keeps probing alternatives.
//   3. Step the sim, append a (obs, visits, reward) tuple, post a render
//      snapshot to main.
// On terminal:
//   - Post tuples + trajectory metadata to main (which forwards tuples
//     to the trainer worker and ingests the trajectory into the pool).
//   - Splice the last K decisions into the failure tape on death/stall/
//     timeout, or into the success tape on flag — either way, identical
//     replays of that line are suppressed in future searches.
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

function buildSim() {
    const lvl = Level.buildLevel({ tileSize: TILE, destructible: true, trackDamagedTiles: false });
    const sim = SwSim.create({
        tilemap: lvl.tilemap,
        spawn: lvl.spawn,
        stompers: lvl.stompers, flyers: lvl.flyers,
        flag: lvl.flag, pickup: lvl.pickup,
        // Long horizon: a complete "traverse → pickup → backtrack-and-
        // destroy → flag" run takes ~600 px / 240 px·s × 3 ≈ 7.5 s of
        // pure travel plus aim/destroy stalls. 600 s gives plenty of
        // room to demonstrate the full loop without timing out.
        timeLimit: 600,
        // 120 decisions ≈ 8 s at the 67 ms cadence. Generous enough that
        // the agent can step back, wait for a flyer to pass, and reposition
        // without the run getting cut for a tactical retreat.
        stallDecisions: 120,
    });
    return { sim, baseSpawnY: lvl.spawn.y, defaultSpawnX: lvl.spawn.x };
}

const { sim, baseSpawnY, defaultSpawnX } = buildSim();

// Two FailureTapes from the grid kit, used as a "history tape" pair —
// one records the tail of failed episodes, the other records the tail of
// flag-success episodes. Both apply the same suppression mechanic
// (per-action prior multiplier capped between `floor` and 1.0). The intent
// is novelty pressure: every search at a familiar (sig, action) is biased
// away from the recorded line regardless of whether that line ended in
// death OR success, so MCTS keeps probing alternatives instead of grinding
// the same trajectory. That gives the trainer richer policy targets across
// episodes.
const failureTape = bro.ai.game.grid.createFailureTape({
    tapeDepth:    8,
    ringCapacity: 200,
    penalty:      0.1,
    floor:        0.001,
});
const successTape = bro.ai.game.grid.createFailureTape({
    tapeDepth:    8,
    ringCapacity: 200,
    penalty:      0.1,
    floor:        0.001,
});
function buildSig() {
    const p = sim.player;
    const col = Math.floor(p.x / TILE);
    const row = Math.floor(p.y / TILE);
    const og  = p.onGround ? 1 : 0;
    const vxSign = p.vx > 8 ? 1 : (p.vx < -8 ? -1 : 0);
    // Include the weapon-cooldown bucket so the same line with a hot
    // beam vs a cool beam isn't treated as identical state.
    const wc = sim.weaponCooldown > 0 ? 1 : 0;
    return col + ',' + row + ',' + og + ',' + vxSign + ',' + wc;
}

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
    sigFn: buildSig,
    // Apply both tapes' multipliers. applyPriors returns a renormalized
    // copy after multiplying; chain so the final prior is multiplied by
    // failure-tape × success-tape suppressors.
    priorAdjust: (sig, prior) => {
        const a = failureTape.applyPriors(sig, prior);
        return successTape.applyPriors(sig, a);
    },
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
            vx: f.vx, animT: f.animT, alive: !!f.alive,
        });
    }
    // Ship the sparse damage diff so main can mirror destruction onto its
    // own tilemap before drawing. damagedDiff is a small Int32Array of
    // [wordIdx, value, ...] pairs — a few KB at most even after heavy
    // carving.
    const dmg = sim.tilemap.damageDiff();
    // Beams fired this decision (0–FRAME_SKIP entries). Plain shape so
    // structured-clone is cheap; main holds them on its own ttl timer
    // for visual decay across multiple render frames.
    const recent = sim.recentBeams;
    const beams = recent.length
        ? recent.map((b) => ({ x0: b.x0, y0: b.y0, x1: b.x1, y1: b.y1 }))
        : null;
    return Object.assign({
        tick: sim.tick,
        player: {
            x: p.x, y: p.y, vx: p.vx, vy: p.vy,
            onGround: !!p.onGround, facing: p.facing,
            w: p.w, h: p.h,
        },
        stompers, flyers,
        pickupCollected: !!sim.pickupCollected,
        hasWeapon: !!sim.hasWeapon,
        weaponCooldown: sim.weaponCooldown | 0,
        phase: sim.phase | 0,
        damageDiff: dmg ? new Int32Array(dmg) : null,
        beams,
        episodes, lastReason,
        bestX: bestXEpisode,
        decisions: decisionsTotal,
        tapeSize: failureTape.size + successTape.size,
        tapeCapacity: failureTape.capacity + successTape.capacity,
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

const TAPE_LOOKBACK = 8;
function endEpisode(reason) {
    const result = agent.endEpisode(reason);
    // Route the tail to the appropriate history tape. Failures suppress
    // the same fatal line; successes suppress the same winning line so
    // future searches must find a *different* path to the flag.
    const targetTape =
        (reason === 'death' || reason === 'stall' || reason === 'timeout') ? failureTape :
        (reason === 'flag') ? successTape :
        null;
    if (targetTape) {
        const len = Math.min(sigList.length, result.actions.length);
        const start = Math.max(0, len - TAPE_LOOKBACK);
        const tail = [];
        for (let i = start; i < len; i++) {
            tail.push({ sig: sigList[i], action: result.actions[i] });
        }
        if (tail.length) targetTape.recordFailure(tail);
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
    sigList.push(buildSig());
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
        failureTape.clear();
        successTape.clear();
    } else if (m.type === 'kill') {
        // User-triggered kill (K key in main). Seal the episode as a
        // death so the failure tape captures the tail, then next doStep
        // applies any pending seed and starts fresh.
        if (inEpisode) endEpisode('death');
    } else if (m.type === 'stop') {
        running = false;
    }
};

// Initial readiness signal so main starts pumping immediately. We may
// not have weights yet — if so doStep above will simply re-post ready.
self.postMessage({ type: 'ready', source: 'live' });
