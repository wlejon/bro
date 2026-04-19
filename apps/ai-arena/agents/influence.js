// agents/influence.js — Influence-map (threat / support / offense) agent.
//
// Each tick, every unit evaluates a ring of candidate cells around its
// current position and scores them by:
//
//   score(c) = support(c)  +  W_OFF * offense(c)  -  W_THR * threat(c)
//
// Where:
//   threat(c)  = sum over enemies of [hasLOS(c→e) * dps_e * exp(-d/THR_SCALE)]
//   support(c) = sum over teammates of [hasLOS(c→t) * dps_t * exp(-d/SUP_SCALE)]
//   offense(c) = sum over enemies of [hasLOS(c→e) * (d ≤ range) * exp(-d/OFF_SCALE)]
//
// The best cell becomes a movement destination, blended 50% with whatever
// the scripted policy wanted. Blending keeps the reactive cover / flee /
// kite behavior intact; influence only biases positioning toward cells
// that are good crossfire positions.
//
// Showcases brogameagent's hasLineOfSight + NavGrid.isWalkable used to
// evaluate a discrete set of tactical positions per agent per tick.
// Because scoring is local (only probes nearby cells), cost stays
// bounded: 12 candidates * 16 LOS checks * 16 agents = ~3K queries per
// 0.25s planning tick.
(function () {
    "use strict";

    var PLAN_HZ        = 4;      // 4 Hz per-agent position refresh
    var PLAN_INTERVAL  = 1 / PLAN_HZ;
    var NUM_ANGLES     = 12;     // candidates per ring
    var RINGS          = [1.5, 3.0];
    var BLEND_WEIGHT   = 0.35;   // nudge toward influence cell; scripted keeps tactical context

    var W_THR          = 1.0;
    var W_SUP          = 0.5;
    var W_OFF          = 2.0;
    var THR_SCALE      = 6.0;    // threat radius (meters)
    var SUP_SCALE      = 5.0;
    var OFF_SCALE      = 5.0;

    function dist(ax, az, bx, bz) { var dx = ax-bx, dz = az-bz; return Math.sqrt(dx*dx + dz*dz); }
    function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

    // Unit DPS estimate — basic attack only. Abilities add instability
    // (cooldowns, mana) that's better captured by the scripted flee/cover
    // logic than baked into the influence field.
    function unitDps(u) { return (u.attacksPerSec || 1.4) * 9; /* basic damage */ }

    function scoreCell(cx, cz, agent, enemies, teammates, obstacles) {
        var threat = 0, support = 0, offense = 0;
        var range = agent.unit.attackRange || 9;
        var radius = agent.unit.radius || 0.4;

        for (var i = 0; i < enemies.length; i++) {
            var e = enemies[i];
            var d = dist(cx, cz, e.x, e.z);
            if (d > 15) continue;                      // far enemies: negligible
            if (!bro.ai.game.hasLineOfSight(cx, cz, e.x, e.z, obstacles)) continue;
            var atten = Math.exp(-d / THR_SCALE);
            threat += unitDps(e.unit) * atten;

            // Offense: prefer cells near the OUTER edge of our attack
            // range over point-blank — minimises opponent lead-aim,
            // matches scripted's range*0.85 stand-off. Closer than
            // range*0.6 is discounted because it's dodging territory
            // where the scripted kite rule also wants to back off.
            if (d <= range + 0.5) {
                var standoff = 1 - Math.abs(d - range * 0.85) / Math.max(0.1, range * 0.5);
                if (standoff < 0) standoff = 0;
                var bonus = unitDps(agent.unit) * standoff;
                if (AI.shared && AI.shared.teamFocus) {
                    var tf = AI.shared.teamFocus[agent.unit.teamId];
                    if (tf && tf.unit.id === e.unit.id) bonus *= 1.35;
                }
                offense += bonus;
            }
        }

        for (var j = 0; j < teammates.length; j++) {
            var t = teammates[j];
            if (t === agent) continue;
            var td = dist(cx, cz, t.x, t.z);
            if (td > 12) continue;
            // Very close teammates reduce support (grenade splash 2.5m is
            // the calibration — blue already pays with AoE damage).
            if (td < 2.5) { support -= 8 * (2.5 - td); continue; }
            if (!bro.ai.game.hasLineOfSight(cx, cz, t.x, t.z, obstacles)) continue;
            support += unitDps(t.unit) * Math.exp(-td / SUP_SCALE);
        }

        // Slight anti-corner penalty: cells very near arena bounds get
        // docked — corners collapse the strafe space needed for dodging.
        var bx = 20 - Math.abs(cx), bz = 20 - Math.abs(cz);
        var cornerPenalty = Math.max(0, 2 - Math.min(bx, bz)) * 5;

        return W_SUP * support + W_OFF * offense - W_THR * threat - cornerPenalty;
    }

    function bestInfluenceCell(agent, enemies, teammates, obstacles, nav) {
        var best = { x: agent.x, z: agent.z }, bestScore = -Infinity;
        // Evaluate current cell first as a baseline.
        bestScore = scoreCell(agent.x, agent.z, agent, enemies, teammates, obstacles);

        for (var ri = 0; ri < RINGS.length; ri++) {
            var r = RINGS[ri];
            // Phase-offset rings so candidates don't align radially.
            var phase = (ri * Math.PI / NUM_ANGLES);
            for (var a = 0; a < NUM_ANGLES; a++) {
                var ang = phase + (a * 2 * Math.PI / NUM_ANGLES);
                var cx = clamp(agent.x + Math.cos(ang) * r, -19, 19);
                var cz = clamp(agent.z + Math.sin(ang) * r, -19, 19);
                if (nav && !nav.isWalkable(cx, cz)) continue;
                var s = scoreCell(cx, cz, agent, enemies, teammates, obstacles);
                if (s > bestScore) { bestScore = s; best = { x: cx, z: cz }; }
            }
        }
        return best;
    }

    // Per-agent scratch: cached influence destination, refresh timer,
    // last committed (rounded, deduped) move target so repeated moveTo
    // calls with near-identical coordinates don't trigger an A* replan
    // every 30 Hz tick.
    var mem = {};
    function getIMem(id) {
        var m = mem[id];
        if (!m) m = mem[id] = {
            destX: null, destZ: null, lastPlanT: -99,
            lastMoveX: null, lastMoveZ: null,
        };
        return m;
    }

    // Last team-level stats (for AGENT STATS panel). Populated during
    // teamTick; read by stats().
    var teamStats = {};

    // Wrap `self` so moveTo calls blend the scripted destination with
    // the influence destination and dedupe. setTarget is expensive (A*
    // replan); firing it every 30 Hz tick with near-identical blended
    // coords produces a jittering path that costs more than it buys.
    function wrapSelf(self, inf, im) {
        if (!inf) return self;
        return {
            agent: self.agent,
            moveTo: function (x, z) {
                var bx = x * (1 - BLEND_WEIGHT) + inf.x * BLEND_WEIGHT;
                var bz = z * (1 - BLEND_WEIGHT) + inf.z * BLEND_WEIGHT;
                // Round to 0.5m grid + dedupe — match the pattern the
                // old groups.js blue used to avoid A* thrash.
                var tx = Math.round(bx * 2) * 0.5;
                var tz = Math.round(bz * 2) * 0.5;
                if (tx === im.lastMoveX && tz === im.lastMoveZ) return;
                im.lastMoveX = tx; im.lastMoveZ = tz;
                self.moveTo(tx, tz);
            },
            flee: function (x, z) {
                // Flee path uses its own cover-aware selection; don't blend.
                im.lastMoveX = null; im.lastMoveZ = null;
                (self.flee || self.moveTo).call(self, x, z);
            },
            cast: function (slot, tid) { self.cast(slot, tid); },
            hold: function (dt)        { self.hold(dt); },
        };
    }

    Agents.register({
        id: "influence",
        label: "Influence maps",

        reset: function () { mem = {}; teamStats = {}; },

        teamTick: function (state, teamId, dt) {
            // Recompute per-agent influence destinations at PLAN_HZ.
            // Doing it in teamTick (once per frame) rather than think()
            // (30 Hz per-agent) amortizes cost + guarantees consistency
            // when multiple agents read the same shared perception.
            var now = state.elapsed;
            var teams = AI.shared.teams;
            var obstacles = AI.shared.obstacles;
            var nav = state.nav;
            var team = teams[teamId];
            var enemies = teams[1 - teamId];
            var sumElapsed = 0, evals = 0;
            var t0 = Date.now();
            for (var i = 0; i < team.length; i++) {
                var a = team[i];
                var im = getIMem(a.unit.id);
                if (now - im.lastPlanT < PLAN_INTERVAL) continue;
                var best = bestInfluenceCell(a, enemies, team, obstacles, nav);
                im.destX = best.x; im.destZ = best.z;
                im.lastPlanT = now;
                evals++;
            }
            sumElapsed = Date.now() - t0;
            teamStats[teamId] = {
                label: "influence [team " + (teamId === 1 ? "blue" : "red") + "]",
                "evals":   evals,
                "plan ms": sumElapsed.toFixed(1),
                "alive":   team.length,
            };
        },

        think: function (self, world) {
            var id = self.agent.unit.id;
            var im = getIMem(id);
            var inf = im.destX !== null ? { x: im.destX, z: im.destZ } : null;
            AI.think(wrapSelf(self, inf, im), world);
        },

        stats: function (state, teamId) {
            return teamStats[teamId] || null;
        },
    });
})();
