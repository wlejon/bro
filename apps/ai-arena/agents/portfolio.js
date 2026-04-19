// agents/portfolio.js — Portfolio script selection via rollout search.
//
// Showcases brogameagent's World::snapshot / restore for branching futures.
// Algorithm:
//
//   Every PLAN_INTERVAL seconds of sim time (teamTick):
//     1. Capture the world snapshot + AI.memory + elapsed time.
//     2. For each candidate script:
//          a. Restore to the snapshot.
//          b. Write the script's tuning into AI.tuningByTeam for the
//             planner's team (opponent team gets default tuning so we
//             simulate against their real scripted policy).
//          c. Step the world for ROLLOUT_STEPS ticks, calling AI.think
//             for every alive agent each tick (both teams run the real
//             scripted policy — this is the opponent model).
//          d. Score = own HP + ally-count bonus  −  enemy HP − enemy-count.
//          e. Record the score.
//     3. Restore the world / memory / time back to the live state.
//     4. Commit the winning script for PLAN_INTERVAL.
//
//   During the committed window, per-agent think() writes the winning
//   script's tuning into AI.tuningByTeam before delegating to scripted
//   think — same code path the search evaluated, so the committed plan
//   executes identically to what was searched.
//
// Scripts are parameterizations of the scripted policy (not wholly new
// behaviors). This keeps the evaluator fair: each rollout tests the same
// mechanics a human would see; only the tuning differs. Extending with
// new scripts is just adding a tuning entry to SCRIPTS.
(function () {
    "use strict";

    // Tuning blocks consumed by AI.think. Keep within the envelope of the
    // scripted baseline — large deviations behave erratically because the
    // cover/flee/kite helpers were tuned for defaults.
    //
    //   default    — baseline scripted.
    //   aggro      — lower flee, closer engage, higher ability spend.
    //   kite       — hold at max range, never kite in, conservative mana.
    //   defensive  — flee early, hoard mana for heals.
    //   burst      — engage close, reserve mana until bursts line up.
    var SCRIPTS = {
        default:   {
            fleeHpFrac: 0.35, engageDistMul: 0.85, kiteDistMul: 0.45,
            supportEngageDistMul: 0.98, supportKiteDistMul: 0.80,
            manaReserveHeal: 25, fireballMinHp: 0.85,
        },
        aggro:     {
            fleeHpFrac: 0.25, engageDistMul: 0.70, kiteDistMul: 0.35,
            supportEngageDistMul: 0.90, supportKiteDistMul: 0.70,
            manaReserveHeal: 25, fireballMinHp: 0.70,
        },
        kite:      {
            fleeHpFrac: 0.45, engageDistMul: 0.98, kiteDistMul: 0.55,
            supportEngageDistMul: 1.02, supportKiteDistMul: 0.90,
            manaReserveHeal: 35, fireballMinHp: 0.90,
        },
        defensive: {
            fleeHpFrac: 0.50, engageDistMul: 0.88, kiteDistMul: 0.50,
            supportEngageDistMul: 0.95, supportKiteDistMul: 0.80,
            manaReserveHeal: 40, fireballMinHp: 0.95,
        },
        burst:     {
            fleeHpFrac: 0.30, engageDistMul: 0.80, kiteDistMul: 0.40,
            supportEngageDistMul: 0.95, supportKiteDistMul: 0.75,
            manaReserveHeal: 55, fireballMinHp: 1.10,  // never spend on fireball
        },
    };
    var SCRIPT_IDS = ["default", "aggro", "kite", "defensive", "burst"];

    var PLAN_INTERVAL = 0.5;     // seconds between plan refreshes
    var ROLLOUT_STEPS = 48;      // * ROLLOUT_DT = 1.0s rollout horizon
    var ROLLOUT_DT = 1 / 48;     // 48 Hz — coarser than 60 Hz sim for speed

    // Per-team planner state. Keyed by teamId so red + blue could both run
    // portfolio concurrently (A/A comparison).
    var planners = {};

    function newPlanner() {
        return {
            committed: "default",
            committedUntil: 0,
            lastScores: {},
            lastElapsedMs: 0,
        };
    }

    // Synthetic `self` used inside rollouts where the AgentBinding is not
    // active. Mirrors the capability surface AI.think expects (moveTo /
    // cast / flee / hold) but dispatches straight to the underlying agent
    // + world, so the scripted policy runs unmodified.
    function synSelf(agent, world) {
        return {
            agent: agent,
            moveTo: function (x, z) { agent.setTarget(x, z); },
            flee:   function (x, z) { agent.setTarget(x, z); },
            cast:   function (slot, tid) { world.resolveAbility(agent, slot, tid); },
            hold:   function (/*dt*/) { agent.clearTarget(); },
        };
    }

    // Deep clone of AI.memory so rollouts can mutate it without bleeding
    // into the live game. BotAim state is plain primitives (yaw, pitch,
    // sampleT, turnSpeed, sampleInterval, fireConeRad) — safe for JSON.
    // Arrays and nested primitives likewise. No circular refs.
    function cloneMemory(mem) {
        return JSON.parse(JSON.stringify(mem));
    }

    // Score team `t` in current world state. Positive = team has advantage.
    // Weights: 1.0 per HP point, 80 per alive unit. HP gives continuous
    // signal even when no kills land in the rollout horizon.
    function teamScore(state, t) {
        var ownHp = 0, oppHp = 0, ownAlive = 0, oppAlive = 0;
        for (var i = 0; i < state.agents.length; i++) {
            var a = state.agents[i];
            if (a.unit.teamId === t) {
                if (a.unit.alive) { ownHp += a.unit.hp; ownAlive++; }
            } else {
                if (a.unit.alive) { oppHp += a.unit.hp; oppAlive++; }
            }
        }
        return (ownHp - oppHp) + (ownAlive - oppAlive) * 80;
    }

    // Run one rollout: apply `scriptId` to `teamId`, default tuning to
    // opponent, step the world ROLLOUT_STEPS times invoking the scripted
    // think for every alive agent each tick. Return score delta (positive
    // = good for `teamId`).
    function runRollout(state, teamId, scriptId) {
        var world = state.world;
        AI.tuningByTeam[teamId]     = SCRIPTS[scriptId];
        AI.tuningByTeam[1 - teamId] = SCRIPTS.default;

        var scoreBefore = teamScore(state, teamId);
        for (var step = 0; step < ROLLOUT_STEPS; step++) {
            AI.updateShared(state);
            for (var i = 0; i < state.agents.length; i++) {
                var a = state.agents[i];
                if (!a.unit.alive) continue;
                AI.think(synSelf(a, world), world);
            }
            world.tick(ROLLOUT_DT);
            state.elapsed += ROLLOUT_DT;
        }
        return teamScore(state, teamId) - scoreBefore;
    }

    // Rollout evaluation of every candidate. Returns { best, scores }.
    function searchBest(state, teamId) {
        var world = state.world;

        // Save live state so rollouts are side-effect-free.
        var snap          = world.snapshot();
        var savedMem      = cloneMemory(AI.memory);
        var savedElapsed  = state.elapsed;
        var savedClaimed  = AI.claimedCover.slice();
        var savedTuning0  = AI.tuningByTeam[0];
        var savedTuning1  = AI.tuningByTeam[1];

        var scores = {};
        for (var s = 0; s < SCRIPT_IDS.length; s++) {
            var id = SCRIPT_IDS[s];
            world.restore(snap);
            AI.memory        = cloneMemory(savedMem);
            AI.claimedCover  = savedClaimed.slice();
            state.elapsed    = savedElapsed;
            scores[id] = runRollout(state, teamId, id);
        }

        // Final restore — back to exactly the pre-search state.
        world.restore(snap);
        AI.memory            = savedMem;
        AI.claimedCover      = savedClaimed;
        state.elapsed        = savedElapsed;
        AI.tuningByTeam[0]   = savedTuning0;
        AI.tuningByTeam[1]   = savedTuning1;

        var best = SCRIPT_IDS[0], bestS = -Infinity;
        for (var j = 0; j < SCRIPT_IDS.length; j++) {
            var k = SCRIPT_IDS[j];
            if (scores[k] > bestS) { bestS = scores[k]; best = k; }
        }
        return { best: best, scores: scores };
    }

    Agents.register({
        id: "portfolio",
        label: "Portfolio (rollout)",

        teamTick: function (state, teamId, dt) {
            var p = planners[teamId] || (planners[teamId] = newPlanner());
            if (state.elapsed < p.committedUntil) return;
            var t0 = (typeof performance !== "undefined" ?
                      performance.now() : Date.now());
            var res = searchBest(state, teamId);
            var t1 = (typeof performance !== "undefined" ?
                      performance.now() : Date.now());
            p.committed      = res.best;
            p.lastScores     = res.scores;
            p.lastElapsedMs  = t1 - t0;
            p.committedUntil = state.elapsed + PLAN_INTERVAL;
        },

        think: function (self, world) {
            // Apply committed tuning, then run scripted think. Both teams'
            // tuning slots are written each call so rollouts and committed
            // execution read from the same place — no stale values bleed in.
            var teamId = self.agent.unit.teamId;
            var p = planners[teamId];
            var scriptId = p ? p.committed : "default";
            AI.tuningByTeam[teamId] = SCRIPTS[scriptId];
            if (!AI.tuningByTeam[1 - teamId]) {
                AI.tuningByTeam[1 - teamId] = SCRIPTS.default;
            }
            AI.think(self, world);
        },

        stats: function (state, teamId) {
            var p = planners[teamId];
            if (!p) return null;
            var out = {
                label:      "portfolio [team " + (teamId === 1 ? "blue" : "red") + "]",
                "script":   p.committed,
                "plan ms":  p.lastElapsedMs.toFixed(1),
                "replan in": (p.committedUntil - state.elapsed).toFixed(2) + "s",
            };
            for (var k in p.lastScores) {
                if (Object.prototype.hasOwnProperty.call(p.lastScores, k)) {
                    out["  " + k] = p.lastScores[k].toFixed(0);
                }
            }
            return out;
        },
    });
})();
