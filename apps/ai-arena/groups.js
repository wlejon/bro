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
            // Realtime-safe budget: tactic + fine total ≤ ~15ms per decide at
            // 4 Hz, so a single rAF frame (~16ms) can absorb the hit without
            // visibly stalling the sim. Rollouts use `scripted` (kite / flee
            // / ability casts) so value estimates match the real opponent
            // instead of the `aggressive` punching bag.
            // Tactic looks deep (80 ticks × 2 actionRepeat × 0.016 ≈ 2.5 s
            // of sim) so Retreat's "pinned against the wall, still getting
            // shot" outcome shows up — short-horizon tactic search sees
            // retreat as free HP preservation and commits to it forever.
            tactic: {
                iterations: 300, budgetMs: 4, rolloutHorizon: 80,
                actionRepeat: 2, tacticWindowDecisions: 2,
            },
            fine: {
                iterations: 1500, budgetMs: 10, rolloutHorizon: 30,
                actionRepeat: 2, priorC: 2.0, pwAlpha: 0.7,
            },
            rolloutPolicy: "aggressive",
            opponentPolicy: "scripted",
            evaluator: "teamPosition",
            // With 66 legal actions per hero (11 MoveDirs × 2 attack opts × 3
            // ability opts on average) and only ~1500 iters split across 8
            // heroes, a weak prior leaves the committed action at the mercy
            // of visit-count noise. Strong tactic bias is required to funnel
            // search into sensible plays. Scatter picks per-hero distinct
            // PathToTarget so clumping isn't a concern.
            tacticMatchWeight: 8.0,
            tacticOtherWeight: 1.0,
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
            // Push the motion target for the new decision window. world.tick
            // steers the agent along this target every frame until the next
            // decision overrides — identical in spirit to how the scripted
            // AgentBinding drives movement via capabilities.
            setTargetFromAction(heroes[i], state.world, joint[i]);
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

    // MoveDir enum mirror (mcts.h). Path* kinds translate to setTarget for
    // live motion; the cardinal dirs translate to a 3-unit waypoint in the
    // enemy-local frame. Kept in sync by review, not by binding.
    var MD_HOLD = 0, MD_N = 1, MD_NE = 2, MD_E = 3, MD_SE = 4,
        MD_S = 5, MD_SW = 6, MD_W = 7, MD_NW = 8,
        MD_PATH_TO = 9, MD_PATH_AWAY = 10;
    var S45 = 0.70710678;
    var MD_VEC = [[0,0],[0,-1],[S45,-S45],[1,0],[S45,S45],[0,1],
                  [-S45,S45],[-1,0],[-S45,-S45]];

    function setTargetFromAction(agent, world, action) {
        var enemy = world.nearestEnemy(agent);
        var md = action.moveDir;
        if (md === MD_HOLD) { agent.clearTarget(); return; }
        if (!enemy || !enemy.unit.alive) { agent.clearTarget(); return; }
        var dx = enemy.x - agent.x, dz = enemy.z - agent.z;
        var d = Math.hypot(dx, dz);
        if (d < 0.01) { agent.clearTarget(); return; }
        if (md === MD_PATH_TO) {
            // Stop at the attack-range edge so the hero pauses to fire
            // instead of charging into point-blank and eating a projectile.
            var r = agent.unit.attackRange || 9;
            var leadDist = Math.min(4.0, Math.max(0, d - r + 1.0));
            if (leadDist < 0.15) { agent.clearTarget(); return; }
            var tx = agent.x + (dx/d) * leadDist;
            var tz = agent.z + (dz/d) * leadDist;
            agent.setTarget(tx, tz);
            return;
        }
        if (md === MD_PATH_AWAY) {
            agent.setTarget(agent.x - (dx/d) * 4.0,
                            agent.z - (dz/d) * 4.0);
            return;
        }
        // Direct 8-way: rotate local frame by aim-toward-enemy yaw.
        // aim yaw follows FPS convention (0 = -Z). Matches mcts::apply.
        var aimYaw = Math.atan2(dx, -dz);
        var c = Math.cos(aimYaw), s = Math.sin(aimYaw);
        var v = MD_VEC[md] || [0, 0];
        var worldDx = v[0] * c + v[1] * (-s);
        var worldDz = v[0] * s + v[1] * c;
        var LEN = 3.0;
        agent.setTarget(agent.x + worldDx * LEN, agent.z + worldDz * LEN);
    }

    // Drive every MCTS-controlled agent. Motion state (setTarget) is pushed
    // only when a new joint action commits (see Groups.tickGroup); combat
    // resolution (auto-attack + ability) fires every frame so cooldown gates
    // inside World::resolveAttack/Ability do the throttling naturally. This
    // splits motion from attack/ability to avoid the old per-frame
    // applyCombatAction double-integration (world.tick already ticks every
    // Agent::update through AIWorldTicker, so an additional mcts::apply per
    // frame was decelerating its own velocity).
    Groups.drive = function (state, dt) {
        if (!state.groups) return;
        var world = state.world;
        for (var i = 0; i < state.agents.length; i++) {
            var a = state.agents[i];
            if (!a.unit.alive) continue;
            var g = state.groups[a.unit.teamId];
            if (!g) continue;
            var action = g.lastActions[a.unit.id];
            if (action) {
                var enemy = world.nearestEnemy(a);
                if (action.attackSlot >= 0 && enemy && enemy.unit.alive) {
                    world.resolveAttack(a, enemy.unit.id);
                }
                if (action.abilitySlot >= 0) {
                    var tid = (enemy && enemy.unit.alive) ? enemy.unit.id : a.unit.id;
                    world.resolveAbility(a, action.abilitySlot, tid);
                }
            }
            // Keep the scene node in sync with the authoritative agent
            // position/facing — no AgentBinding is attached to do it.
            var node = Scene3D.units[a.unit.id];
            if (node) {
                node.x = a.x;
                node.y = Scene3D.UNIT_Y;
                node.z = a.z;
                node.rotationY = -a.yaw;
            }
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
