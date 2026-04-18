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
            // Horizons intentionally long — Retreat only looks free at short
            // horizons; at 2–4 s of sim the pin-against-wall death shows up.
            tactic: {
                iterations: 400, budgetMs: 20, rolloutHorizon: 60,
                actionRepeat: 4, tacticWindowDecisions: 3,
            },
            fine: {
                iterations: 1200, budgetMs: 30, rolloutHorizon: 40,
                actionRepeat: 4, priorC: 2.0, pwAlpha: 0.65,
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

    // Drive every MCTS-controlled agent through the exact same mcts::apply
    // path used by rollouts: motion + auto-attack (hitscan via resolveAttack)
    // + ability dispatch, all in one native call. Called once per rAF frame
    // with the real delta; the cached CombatAction is replayed across frames
    // until the planner emits a new one (matches the rollout's
    // "hold action across action_repeat ticks" semantics).
    Groups.drive = function (state, dt) {
        if (!state.groups) return;
        var world = state.world;
        for (var i = 0; i < state.agents.length; i++) {
            var a = state.agents[i];
            if (!a.unit.alive) continue;
            var g = state.groups[a.unit.teamId];
            if (!g) continue;
            var action = g.lastActions[a.unit.id];
            if (!action) continue;
            bro.ai.game.applyCombatAction(a, world, action, dt);
        }
    };

    // ── Binding toggle ────────────────────────────────────────────────
    // MCTS-controlled agents must have their scene-side AgentBinding detached
    // so the capability layer (setTarget/path-following inside world.tick)
    // doesn't fight the applyCombatAction integration we do each frame.
    // Flipping back to scripted re-attaches with the original think config.
    var DEFAULT_CAPS = ["move_to", "cast_ability", "flee", "hold", "aimed_shot"];

    function attachScripted(state, agent) {
        var node = Scene3D.units[agent.unit.id];
        if (!node) return;
        try { node.detachAgent(); } catch (e) {}
        node.attachAgent(state.world, agent, {
            capabilities: DEFAULT_CAPS,
            thinkHz: 30,
            faceMovement: true,
            yOffset: Scene3D.UNIT_Y,
            think: AI.think,
        });
    }

    function detachAgentSafe(agent) {
        var node = Scene3D.units[agent.unit.id];
        if (!node) return;
        try { node.detachAgent(); } catch (e) {}
        // Clear any pending scripted seek so world.tick's update() is a
        // true no-op for this agent — applyCombatAction owns motion now.
        try { agent.clearTarget(); } catch (e) {}
    }

    // Apply the current mode to a team. "mcts" detaches bindings; any other
    // value (scripted) re-attaches them. Idempotent.
    Groups.applyModeForTeam = function (state, teamId, mode) {
        for (var i = 0; i < state.agents.length; i++) {
            var a = state.agents[i];
            if (a.unit.teamId !== teamId) continue;
            if (mode === "mcts") detachAgentSafe(a);
            else                 attachScripted(state, a);
        }
    };
})();
