// groups.js — one LayeredPlanner per team ("group"). The planner's
// TacticMcts picks a coarse team tactic (Hold / FocusLowestHp / Scatter /
// Retreat) every `tacticWindowDecisions` fine windows; its inner TeamMcts
// then searches a joint CombatAction per living member every decide() call,
// priored toward the committed tactic. We tick decide() at Groups.HZ; the
// per-agent think() replays the last cached action each of its ticks.
//
// Groups are created lazily per team the first time a team flips to mcts
// mode, so the scripted path stays allocation-free.
var Groups = {};
(function () {
    "use strict";

    // Decision cadence. 4 Hz matches tactic_window_decisions × action_repeat
    // × sim_dt ≈ human-legible coordination (retarget, push, retreat).
    Groups.HZ = 4;
    Groups.INTERVAL = 1 / Groups.HZ;

    // How far forward a MoveDir::N step carries the agent before we
    // recompute. Must be long enough that an A*-pathed waypoint is distinct
    // from the agent's own cell; short enough that a stale direction doesn't
    // leave the agent committed to a bad move across a full window.
    Groups.STEP = 3.0;

    function mkPlanner() {
        return bro.ai.game.createLayeredPlanner({
            tactic: {
                iterations: 240, budgetMs: 6, rolloutHorizon: 12,
                actionRepeat: 4, tacticWindowDecisions: 3,
            },
            fine: {
                iterations: 480, budgetMs: 8, rolloutHorizon: 10,
                actionRepeat: 4, priorC: 2.0,
            },
            rolloutPolicy: "aggressive",
            opponentPolicy: "aggressive",
            evaluator: "teamHpDelta",
        });
    }

    // state.groups is keyed by teamId. Created on demand by ensure().
    Groups.ensure = function (state, teamId) {
        if (!state.groups) state.groups = {};
        var g = state.groups[teamId];
        if (!g) {
            g = {
                teamId: teamId,
                planner: mkPlanner(),
                accum: Groups.INTERVAL,   // fire on first tick
                lastActions: {},           // agentId → CombatAction
                decidedAtT: -1,
            };
            state.groups[teamId] = g;
        }
        return g;
    };

    Groups.reset = function (state) {
        state.groups = {};
    };

    // Collect living agents of a team as an array. Order matters — the
    // planner returns a parallel array we zip back by index.
    function livingOf(state, teamId) {
        var out = [];
        for (var i = 0; i < state.agents.length; i++) {
            var a = state.agents[i];
            if (a.unit.teamId === teamId && a.unit.alive) out.push(a);
        }
        return out;
    }

    // Tick one group's planner if its accumulator has rolled over. Publishes
    // fresh stats to state.lastMctsStats so the HUD picks them up on the
    // next render frame.
    Groups.tickGroup = function (state, g, dt) {
        g.accum += dt;
        if (g.accum < Groups.INTERVAL) return false;
        g.accum = 0;

        var heroes = livingOf(state, g.teamId);
        if (heroes.length === 0) { g.lastActions = {}; return false; }

        var joint = g.planner.decide(state.world, heroes);
        var next = {};
        for (var i = 0; i < heroes.length; i++) {
            next[heroes[i].unit.id] = joint[i];
        }
        g.lastActions = next;
        g.decidedAtT = state.elapsed;

        var s = g.planner.lastStats;
        state.lastMctsStats = {
            teamId: g.teamId,
            tactic: s.committedTactic.kind,
            windowsUntilReplan: s.windowsUntilReplan,
            replanned: s.replannedThisCall,
            iterations: s.fineStats.iterations,
            bestVisits: s.fineStats.bestVisits,
            bestMean: s.fineStats.bestMean,
            elapsedMs: s.fineStats.elapsedMs + s.tacticStats.elapsedMs,
        };
        return true;
    };

    // Tick all active groups. Called once per rAF frame from main.
    Groups.tick = function (state, dt) {
        if (!state.groups) return;
        for (var k in state.groups) {
            if (!state.groups.hasOwnProperty(k)) continue;
            Groups.tickGroup(state, state.groups[k], dt);
        }
    };

    Groups.actionFor = function (state, teamId, agentId) {
        if (!state.groups || !state.groups[teamId]) return null;
        return state.groups[teamId].lastActions[agentId] || null;
    };

    // MoveDir → world-space unit vector in the agent's aim frame.
    //   N (forward) = toward aim target = (sin(yaw), -cos(yaw))
    //   E (right)   = forward rotated 90° CW = (-fz, fx)
    // Any fixed aim direction stays coherent across a decision window; the
    // aim_yaw reset inside mcts::apply keeps the policy/integrator frames
    // aligned, so what the planner sees in rollout matches what we compute
    // here at apply time.
    var S = Math.SQRT1_2;
    Groups.moveDirVector = function (moveDir, yaw) {
        if (moveDir === 0) return { x: 0, z: 0 };
        var fx = Math.sin(yaw), fz = -Math.cos(yaw);
        var rx = -fz, rz = fx;
        switch (moveDir) {
            case 1: return { x: fx,         z: fz };          // N
            case 2: return { x: fx*S+rx*S,  z: fz*S+rz*S };   // NE
            case 3: return { x: rx,         z: rz };          // E
            case 4: return { x: -fx*S+rx*S, z: -fz*S+rz*S };  // SE
            case 5: return { x: -fx,        z: -fz };         // S
            case 6: return { x: -fx*S-rx*S, z: -fz*S-rz*S };  // SW
            case 7: return { x: -rx,        z: -rz };         // W
            case 8: return { x: fx*S-rx*S,  z: fz*S-rz*S };   // NW
        }
        return { x: 0, z: 0 };
    };
})();
