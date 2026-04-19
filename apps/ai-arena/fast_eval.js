// fast_eval.js — Headless match evaluator that bypasses the scene.
//
// The standard eval drives the sim through Scene3D.attachAIWorld which is
// paced by the render loop (≈1x real-time even under advanceTime). That
// makes 30-match evals prohibitive.
//
// This script re-uses the live JS modules (Arena.build, the Agents
// registry, Bot) but never attaches to the scene. A tight while-loop
// calls the active agent's think() and then world.tick() directly, so
// matches run at pure JS speed.
//
// Invoke:
//   bro-headless apps/ai-arena -e "EVAL_MATCHES=30; EVAL_BLUE='portfolio';" \
//       apps/ai-arena/fast_eval.js
//
// Overrideable globals (set via -e BEFORE the script path):
//   EVAL_MATCHES     number of matches  (default 10)
//   EVAL_SECONDS     match-second cap   (default 45)
//   EVAL_RED         red agent id       (default "scripted")
//   EVAL_BLUE        blue agent id      (default "scripted")
//   EVAL_SEED_BASE   world-seed offset  (default 1)
"use strict";

var MATCH_SECONDS = typeof EVAL_SECONDS   === "number" ? EVAL_SECONDS   : 45;
var NUM_MATCHES   = typeof EVAL_MATCHES   === "number" ? EVAL_MATCHES   : 10;
var RED_AI        = typeof EVAL_RED       === "string" ? EVAL_RED       : "scripted";
var BLUE_AI       = typeof EVAL_BLUE      === "string" ? EVAL_BLUE      : "scripted";
var SEED_BASE     = typeof EVAL_SEED_BASE === "number" ? EVAL_SEED_BASE : 1;

var SIM_DT = 1 / 60;
// Think stride: 30 Hz (match the scene's default AgentBinding thinkHz so
// the policies run with the same reaction budget they see in-game).
var THINK_STRIDE = 2;
// Terminal check stride: 0.5 s between checks so short final frames don't
// overshoot the victory condition.
var TERMINAL_STRIDE = 30;

function synSelf(agent, world) {
    return {
        agent: agent,
        moveTo: function (x, z) { agent.setTarget(x, z); },
        flee:   function (x, z) { agent.setTarget(x, z); },
        cast:   function (slot, tid) { world.resolveAbility(agent, slot, tid); },
        hold:   function (/*dt*/) { agent.clearTarget(); },
    };
}

function aliveOnTeam(state, teamId) {
    var n = 0;
    for (var i = 0; i < state.agents.length; i++) {
        var a = state.agents[i];
        if (a.unit.teamId === teamId && a.unit.alive) n++;
    }
    return n;
}

function totalHpOnTeam(state, teamId) {
    var hp = 0;
    for (var i = 0; i < state.agents.length; i++) {
        var a = state.agents[i];
        if (a.unit.teamId === teamId && a.unit.alive) hp += a.unit.hp;
    }
    return hp;
}

// Fresh sim state for one match. Rebuilds the world + agents + abilities;
// never touches Scene3D. We still overwrite App.state so Agents.thinkFor
// (which reads App.state.redAi / blueAi) routes to the configured agents.
function buildMatch(scenario, seed) {
    var built = Arena.build(scenario);
    built.world.seed(seed);
    var state = {
        nav:      built.nav,
        world:    built.world,
        agents:   built.agents,
        byId:     built.byId,
        elapsed:  0,
        redAi:    RED_AI,
        blueAi:   BLUE_AI,
        simSteps: 0,
    };
    // Reset per-unit AI memory (threat trackers, BotAim, flee latches)
    // so the previous match's state doesn't bleed in.
    AI.memory = {};
    AI.claimedCover = [];
    AI.tuningByTeam = [null, null];
    Agents.resetAll();
    App.state = state;
    return state;
}

function runMatch(matchIdx) {
    var scenario = Scenarios.ALL[matchIdx % Scenarios.ALL.length];
    var state = buildMatch(scenario, SEED_BASE + matchIdx);
    var world = state.world;

    var steps = Math.ceil(MATCH_SECONDS / SIM_DT);
    var winner = -1;
    var t0 = Date.now();

    for (var k = 0; k < steps; k++) {
        // Decision + planner step (every THINK_STRIDE sim ticks).
        // Iterate in alternating order each think tick so neither team
        // gets systematic first-mover advantage. In the scene-driven
        // path AgentBinding think() is scheduled across teams evenly;
        // here we have to emulate that fairness manually or one team's
        // projectiles consistently hit first and cascade cover/flee
        // reactions on the other team.
        if (k % THINK_STRIDE === 0) {
            var decisionDt = SIM_DT * THINK_STRIDE;
            AI.updateShared(state);
            Agents.tickTeams(state, decisionDt);
            // Two-phase think: capture every agent's intended moveTo /
            // cast BEFORE any state mutation, then apply them all. That
            // eliminates first-mover advantage entirely — both teams
            // observe the same world snapshot when deciding, and casts
            // spawn simultaneously so projectiles from red and blue are
            // born at the same sim-tick regardless of iteration order.
            var pending = [];
            function capturingSelf(a) {
                return {
                    agent: a,
                    moveTo: function (x, z) { pending.push({ a: a, op: "move", x: x, z: z }); },
                    flee:   function (x, z) { pending.push({ a: a, op: "move", x: x, z: z }); },
                    cast:   function (slot, tid) { pending.push({ a: a, op: "cast", slot: slot, tid: tid }); },
                    hold:   function () { pending.push({ a: a, op: "hold" }); },
                };
            }
            for (var i = 0; i < state.agents.length; i++) {
                var a = state.agents[i];
                if (!a.unit.alive) continue;
                Agents.thinkFor(capturingSelf(a), world);
            }
            // Apply captured commands. Basic-shot projectiles were already
            // spawned as side effects inside Bot.tick; that's inherent to
            // the reflex-robot's firing model. The two-phase gate here
            // controls only the capability-level moves/casts.
            for (var q = 0; q < pending.length; q++) {
                var op = pending[q];
                if (!op.a.unit.alive) continue;
                if (op.op === "move") op.a.setTarget(op.x, op.z);
                else if (op.op === "cast") world.resolveAbility(op.a, op.slot, op.tid);
                else if (op.op === "hold") op.a.clearTarget();
            }
        }

        // Physics / projectile advance + per-agent update (path follow).
        world.tick(SIM_DT);
        state.elapsed += SIM_DT;
        state.simSteps = k + 1;

        // Drain damage events into the threat tracker so scripted's cover
        // / flee latches react to incoming damage the same way they do
        // during real-time play.
        var evs = world.events;
        for (var e = 0; e < evs.length; e++) {
            var ev = evs[e];
            AI.recordDamage(ev.targetId, ev.attackerId, ev.amount, state.elapsed);
        }
        world.clearEvents();

        // Terminal check.
        if (k % TERMINAL_STRIDE === 0) {
            var rN = aliveOnTeam(state, 0), bN = aliveOnTeam(state, 1);
            if (rN === 0 && bN === 0) { winner = -1; break; }
            if (rN === 0) { winner = 1; break; }
            if (bN === 0) { winner = 0; break; }
        }
    }

    if (winner < 0) {
        var rh = totalHpOnTeam(state, 0), bh = totalHpOnTeam(state, 1);
        if (bh > rh * 1.05) winner = 1;
        else if (rh > bh * 1.05) winner = 0;
    }

    return {
        scenario:  scenario.name,
        winner:    winner,
        redAlive:  aliveOnTeam(state, 0),
        blueAlive: aliveOnTeam(state, 1),
        redHp:     totalHpOnTeam(state, 0),
        blueHp:    totalHpOnTeam(state, 1),
        elapsed:   state.elapsed.toFixed(1),
        wallMs:    Date.now() - t0,
    };
}

console.log("==== fast eval: red=" + RED_AI + " vs blue=" + BLUE_AI + " ====");
console.log("matches=" + NUM_MATCHES + " matchSeconds=" + MATCH_SECONDS);

var redWins = 0, blueWins = 0, draws = 0;
var totalWallMs = 0, totalSimSec = 0;
for (var m = 0; m < NUM_MATCHES; m++) {
    var r = runMatch(m);
    totalWallMs += r.wallMs;
    totalSimSec += parseFloat(r.elapsed);
    if (r.winner === 1) blueWins++;
    else if (r.winner === 0) redWins++;
    else draws++;
    console.log("[" + (m+1) + "/" + NUM_MATCHES + "] " + r.scenario +
        "  winner=" + (r.winner === 1 ? "BLUE" : r.winner === 0 ? "RED" : "DRAW") +
        "  red=" + r.redAlive + "(" + r.redHp.toFixed(0) + "hp)" +
        "  blue=" + r.blueAlive + "(" + r.blueHp.toFixed(0) + "hp)" +
        "  t=" + r.elapsed + "s  wall=" + r.wallMs + "ms");
}

var speedup = totalSimSec / (totalWallMs / 1000);
console.log("==== summary ====");
console.log("BLUE (" + BLUE_AI + "): " + blueWins + "/" + NUM_MATCHES +
            "   (" + (100 * blueWins / NUM_MATCHES).toFixed(1) + "%)");
console.log("RED  (" + RED_AI  + "): " + redWins  + "/" + NUM_MATCHES +
            "   (" + (100 * redWins  / NUM_MATCHES).toFixed(1) + "%)");
console.log("DRAW: " + draws + "/" + NUM_MATCHES);
console.log("speedup: " + speedup.toFixed(1) + "x  (wall=" +
            (totalWallMs / 1000).toFixed(1) + "s  sim=" + totalSimSec.toFixed(1) + "s)");
