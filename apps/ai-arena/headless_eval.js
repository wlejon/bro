// headless_eval.js — Run N matches and tally winrate. Reads sel-red-ai /
// sel-blue-ai for algorithm selection; defaults to scripted-vs-scripted
// baseline. Invoke via:
//   bro-headless apps/ai-arena apps/ai-arena/headless_eval.js
//   bro-headless apps/ai-arena apps/ai-arena/headless_eval.js -e "EVAL_BLUE='portfolio'"
//
// Score = team with survivors. Timeout falls back to surviving HP.

"use strict";

var MATCH_SECONDS = typeof EVAL_SECONDS === "number" ? EVAL_SECONDS : 45;
var NUM_MATCHES   = typeof EVAL_MATCHES === "number" ? EVAL_MATCHES : 6;
var TICK_MS       = 500;
var RED_AI        = typeof EVAL_RED  === "string" ? EVAL_RED  : "scripted";
var BLUE_AI       = typeof EVAL_BLUE === "string" ? EVAL_BLUE : "scripted";

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
    var scn = Scenarios.ALL[matchIdx % Scenarios.ALL.length];
    App.setScenario(scn);

    App.state.redAi  = RED_AI;
    App.state.blueAi = BLUE_AI;

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
        var rh = totalHp(App.state, 0), bh = totalHp(App.state, 1);
        if (bh > rh * 1.05) winner = 1;
        else if (rh > bh * 1.05) winner = 0;
    }
    return {
        scenario: scn.name, winner: winner,
        redAlive: teamAlive(App.state, 0),
        blueAlive: teamAlive(App.state, 1),
        redHp: totalHp(App.state, 0),
        blueHp: totalHp(App.state, 1),
        elapsed: App.state.elapsed.toFixed(1),
        wallMs: Date.now() - t0,
    };
}

console.log("==== headless eval: red=" + RED_AI + " vs blue=" + BLUE_AI + " ====");
console.log("matches=" + NUM_MATCHES + " matchSeconds=" + MATCH_SECONDS);

var results = [];
var redWins = 0, blueWins = 0, draws = 0;
for (var m = 0; m < NUM_MATCHES; m++) {
    var r = runMatch(m);
    results.push(r);
    if (r.winner === 1) blueWins++;
    else if (r.winner === 0) redWins++;
    else draws++;
    console.log("[" + (m+1) + "/" + NUM_MATCHES + "] " + r.scenario +
        "  winner=" + (r.winner === 1 ? "BLUE" : r.winner === 0 ? "RED" : "DRAW") +
        "  red=" + r.redAlive + "(" + r.redHp.toFixed(0) + "hp)" +
        "  blue=" + r.blueAlive + "(" + r.blueHp.toFixed(0) + "hp)" +
        "  t=" + r.elapsed + "s  wall=" + r.wallMs + "ms");
}

console.log("==== summary ====");
console.log("BLUE (" + BLUE_AI + "): " + blueWins + "/" + NUM_MATCHES);
console.log("RED  (" + RED_AI  + "): " + redWins  + "/" + NUM_MATCHES);
console.log("DRAW: " + draws + "/" + NUM_MATCHES);
console.log("blue win rate: " + (100 * blueWins / NUM_MATCHES).toFixed(1) + "%");
