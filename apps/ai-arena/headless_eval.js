// headless_eval.js — run N matches of MCTS (blue) vs scripted (red) inside
// the live ai-arena app, headless. Used to iterate on LayeredPlanner tuning
// until MCTS wins 100%. Invoke via:
//   bro-headless apps/ai-arena apps/ai-arena/headless_eval.js
//
// Reads (and mutates) the globals the app installs: App, Scenarios, Groups.
//
// Method: for each match, force blueAi=mcts, tick virtual time up to
// MATCH_SECONDS; stop early once one team is fully dead. Score = team with
// survivors. Draw counts as a loss for MCTS (we want 100%).

"use strict";

var MATCH_SECONDS = 45;          // cap per match
var NUM_MATCHES   = 10;          // baseline run length
var TICK_MS       = 500;         // virtual-time chunk per inner loop

function teamAlive(state, teamId) {
    var n = 0;
    for (var i = 0; i < state.agents.length; i++) {
        var a = state.agents[i];
        if (a.unit.teamId === teamId && a.unit.alive) n++;
    }
    return n;
}

function totalHp(state, teamId) {
    var hp = 0;
    for (var i = 0; i < state.agents.length; i++) {
        var a = state.agents[i];
        if (a.unit.teamId === teamId && a.unit.alive) hp += a.unit.hp;
    }
    return hp;
}

function runMatch(matchIdx) {
    // Cycle scenarios for variety.
    var scn = Scenarios.ALL[matchIdx % Scenarios.ALL.length];
    App.setScenario(scn);

    // Force blue (team 1) onto MCTS — same hook Controls.change runs.
    App.state.blueAi = "mcts";
    Groups.ensure(App.state, 1);
    Groups.applyModeForTeam(App.state, 1, "mcts");

    var t0 = Date.now();
    var winner = -1;
    var steps = Math.ceil(MATCH_SECONDS * 1000 / TICK_MS);
    for (var k = 0; k < steps; k++) {
        advanceTime(TICK_MS);
        var redN = teamAlive(App.state, 0);
        var blueN = teamAlive(App.state, 1);
        if (redN === 0 && blueN === 0) { winner = -1; break; }
        if (redN === 0) { winner = 1; break; }
        if (blueN === 0) { winner = 0; break; }
    }
    if (winner < 0) {
        // Timeout — assign win by higher surviving HP; tie counts as draw.
        var rh = totalHp(App.state, 0), bh = totalHp(App.state, 1);
        if (bh > rh * 1.05) winner = 1;
        else if (rh > bh * 1.05) winner = 0;
    }
    var wallMs = Date.now() - t0;
    return {
        scenario: scn.name, winner: winner,
        redAlive: teamAlive(App.state, 0),
        blueAlive: teamAlive(App.state, 1),
        redHp: totalHp(App.state, 0),
        blueHp: totalHp(App.state, 1),
        elapsed: App.state.elapsed.toFixed(1),
        wallMs: wallMs,
    };
}

console.log("==== headless MCTS eval ====");
console.log("matches=" + NUM_MATCHES + " matchSeconds=" + MATCH_SECONDS);

var results = [];
var mctsWins = 0, scriptedWins = 0, draws = 0;
for (var m = 0; m < NUM_MATCHES; m++) {
    var r = runMatch(m);
    results.push(r);
    if (r.winner === 1) mctsWins++;
    else if (r.winner === 0) scriptedWins++;
    else draws++;
    console.log("[" + (m+1) + "/" + NUM_MATCHES + "] " + r.scenario +
        "  winner=" + (r.winner === 1 ? "MCTS" : r.winner === 0 ? "SCRIPTED" : "DRAW") +
        "  red=" + r.redAlive + "(" + r.redHp.toFixed(0) + "hp)" +
        "  blue=" + r.blueAlive + "(" + r.blueHp.toFixed(0) + "hp)" +
        "  t=" + r.elapsed + "s  wall=" + r.wallMs + "ms");
}

console.log("==== summary ====");
console.log("MCTS:     " + mctsWins + "/" + NUM_MATCHES);
console.log("SCRIPTED: " + scriptedWins + "/" + NUM_MATCHES);
console.log("DRAW:     " + draws + "/" + NUM_MATCHES);
console.log("win rate: " + (100 * mctsWins / NUM_MATCHES).toFixed(1) + "%");
