// ai.js — Shared per-frame AI state + a library of helpers (memory, cover
// search, teammate spacing, team-focus selection, target scoring) that the
// registered agents (scripted, tactical, options_*) and the reflex robot
// (bot.js) pull from.
//
// Every live execution path — firing, movement, ability autocast — runs
// through bot.js. AI.think USED to drive scripted directly; that routing
// moved into agents/scripted.js + Bot. What's kept here is strictly the
// shared scaffolding: the per-agent memory struct (including BotAim state
// and threat tracking), AI.shared populated once per frame by
// AI.updateShared, and pure utility functions the planners reuse.
//
// AI.recordDamage is called from loop.js / fast_eval.js when a DamageEvent
// fires so a target's mem.threat / mem.threatSourceId / mem.lastHitT latch
// correctly — scripted's "seek cover under fire" branch reads those.
var AI = {};
(function () {
    "use strict";

    function dist2(ax, az, bx, bz) { var dx = ax-bx, dz = az-bz; return dx*dx + dz*dz; }
    function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

    // ── Per-agent memory ─────────────────────────────────────────────────
    //
    // Holds everything that needs to persist between think ticks: the
    // BotAim gun-lag state (owned by bot.js), firing cooldown mirror,
    // ability cooldown mirrors, threat tracking, cover/flee latches, and
    // — when running through Bot.tick — the per-hero command queue.
    AI.memory = {};
    AI.getMem = function (id) {
        var m = AI.memory[id];
        if (!m) m = AI.memory[id] = {
            intent: "idle", targetId: null,
            shootCd: 0, lastThinkT: -1,
            aim: BotAim.create({ turnSpeed: 5.0, sampleHz: 15, fireConeRad: 0.15 }),
            threat: 0, threatSourceId: -1, lastHitT: -99,
            role: "front", coverX: null, coverZ: null, coverPickedT: -99,
            fleeX: null, fleeZ: null, fleePickedT: -99,
            perpSign: 1, lastFlip: -99,
            abCd: [0, 0, 0, 0, 0, 0, 0, 0],
        };
        return m;
    };

    AI.recordDamage = function (targetId, attackerId, amount, simT) {
        var m = AI.getMem(targetId);
        m.threat += amount;
        m.lastHitT = simT;
        if (attackerId !== m.threatSourceId) {
            if (m.threatSourceId < 0 || amount * 2 > m.threat) {
                m.threatSourceId = attackerId;
            }
        }
    };

    // Per-frame fan-out: rally/cover pickers append claimed cells so
    // downstream agents avoid piling onto the same spot. Reset each frame.
    AI.claimedCover = [];
    AI.resetClaimedCover = function () { AI.claimedCover.length = 0; };

    // ── Perception: team focus + per-agent target ────────────────────────

    AI.chooseTeamFocus = function (team, enemies, obstacles) {
        var best = null, bestHp = Infinity;
        var PERCEPT_MUL = 1.8;
        for (var i = 0; i < enemies.length; i++) {
            var e = enemies[i];
            var seen = false;
            for (var j = 0; j < team.length; j++) {
                var t = team[j];
                var perceptR = (t.unit.attackRange || 9) * PERCEPT_MUL;
                if (dist2(t.x, t.z, e.x, e.z) > perceptR * perceptR) continue;
                if (bro.ai.game.hasLineOfSight(t.x, t.z, e.x, e.z, obstacles)) {
                    seen = true; break;
                }
            }
            if (!seen) continue;
            if (e.unit.hp < bestHp) { bestHp = e.unit.hp; best = e; }
        }
        if (best) return best;
        // Fallback: nearest enemy to any teammate (ignores LOS).
        var bestD = Infinity;
        for (var k = 0; k < enemies.length; k++) {
            var en = enemies[k];
            for (var m = 0; m < team.length; m++) {
                var d = dist2(team[m].x, team[m].z, en.x, en.z);
                if (d < bestD) { bestD = d; best = en; }
            }
        }
        return best;
    };

    AI.pickTargetFor = function (agent, enemies, teamFocus, obstacles) {
        var best = null, bestScore = -Infinity;
        var ax = agent.x, az = agent.z;
        for (var i = 0; i < enemies.length; i++) {
            var e = enemies[i];
            var dx = e.x - ax, dz = e.z - az;
            var d = Math.sqrt(dx*dx + dz*dz);
            if (d > 18) continue;
            if (!bro.ai.game.hasLineOfSight(ax, az, e.x, e.z, obstacles)) continue;
            var score = -d - e.unit.hp * 0.04;
            if (score > bestScore) { bestScore = score; best = e; }
        }
        return best || teamFocus;
    };

    // ── Movement helpers (cover search, nearest walkable, spacing) ──────

    AI.findWalkableNear = function (nav, x, z, maxR) {
        if (nav.isWalkable(x, z)) return { x: x, z: z };
        for (var r = 0.5; r <= (maxR || 4); r += 0.5) {
            for (var ang = 0; ang < Math.PI * 2; ang += Math.PI / 4) {
                var nx = x + Math.cos(ang) * r, nz = z + Math.sin(ang) * r;
                if (nav.isWalkable(nx, nz)) return { x: nx, z: nz };
            }
        }
        return null;
    };

    // Find a walkable cell that breaks LOS from every listed threat.
    // opts.anchorX/anchorZ centers the ring search; opts.claimed avoids
    // picks already taken by teammates this frame; opts.minThreatDistance
    // rejects cells too close to any threat. Returns {x,z} or null.
    AI.findCover = function (agent, threats, obstacles, nav, opts) {
        if (!threats || threats.length === 0) return null;
        opts = opts || {};
        var ax = agent.x, az = agent.z;
        var cx0 = opts.anchorX !== undefined ? opts.anchorX : ax;
        var cz0 = opts.anchorZ !== undefined ? opts.anchorZ : az;
        var rings = opts.anchorX !== undefined
            ? [1.5, 2.5, 3.5, 5.0] : [2.0, 3.5, 5.0];
        var claimed = opts.claimed || null;
        var minThD2 = opts.minThreatDistance
            ? opts.minThreatDistance * opts.minThreatDistance : 0;

        var best = null, bestScore = -Infinity;
        for (var ri = 0; ri < rings.length; ri++) {
            var r = rings[ri];
            for (var ang = 0; ang < Math.PI * 2; ang += Math.PI / 6) {
                var cx = clamp(cx0 + Math.cos(ang) * r, -19, 19);
                var cz = clamp(cz0 + Math.sin(ang) * r, -19, 19);
                if (!nav.isWalkable(cx, cz)) continue;
                if (claimed) {
                    var tooClose = false;
                    for (var ci = 0; ci < claimed.length; ci++) {
                        var cl = claimed[ci];
                        if ((cx - cl.x) * (cx - cl.x) + (cz - cl.z) * (cz - cl.z) < 1.0) {
                            tooClose = true; break;
                        }
                    }
                    if (tooClose) continue;
                }
                var coversAll = true, tooCloseToThreat = false;
                for (var ti = 0; ti < threats.length; ti++) {
                    var th = threats[ti];
                    if (minThD2 > 0) {
                        var tdx = cx - th.x, tdz = cz - th.z;
                        if (tdx*tdx + tdz*tdz < minThD2) { tooCloseToThreat = true; break; }
                    }
                    if (bro.ai.game.hasLineOfSight(cx, cz, th.x, th.z, obstacles)) {
                        coversAll = false; break;
                    }
                }
                if (tooCloseToThreat || !coversAll) continue;
                var score = -Math.hypot(cx - ax, cz - az);
                if (opts.anchorX !== undefined) score -= 0.4 * Math.hypot(cx - cx0, cz - cz0);
                if (score > bestScore) { bestScore = score; best = { x: cx, z: cz }; }
            }
        }
        return best;
    };

    AI.spaceFromTeammates = function (agent, teammates, x, z) {
        var SPACING = 1.4;
        for (var ti = 0; ti < teammates.length; ti++) {
            var tm = teammates[ti];
            if (tm === agent) continue;
            var ddx = x - tm.x, ddz = z - tm.z;
            var dd = Math.hypot(ddx, ddz);
            if (dd < SPACING && dd > 0.01) {
                var push = SPACING - dd;
                x += (ddx / dd) * push;
                z += (ddz / dd) * push;
            }
        }
        return { x: clamp(x, -19, 19), z: clamp(z, -19, 19) };
    };

    // ── Shared per-frame state ───────────────────────────────────────────

    AI.shared = {
        world: null, nav: null, obstacles: null,
        teams: [[], []], teamFocus: [null, null], simT: 0, byId: null,
    };

    // Per-team "mood" tunings. Portfolio-style agents can poke
    // AI.tuningByTeam to bias scripted toward a candidate strategy; missing
    // team falls back to DEFAULT_TUNING.
    AI.DEFAULT_TUNING = {
        fleeHpFrac: 0.35,
        engageDistMul: 0.85,
        kiteDistMul: 0.45,
        supportEngageDistMul: 0.98,
        supportKiteDistMul: 0.80,
        manaReserveHeal: 25,
        fireballMinHp: 0.85,
    };
    AI.tuningByTeam = [null, null];
    AI.tuningFor = function (teamId) {
        return AI.tuningByTeam[teamId] || AI.DEFAULT_TUNING;
    };

    AI.updateShared = function (state) {
        AI.resetClaimedCover();
        AI.shared.world = state.world;
        AI.shared.nav = state.nav;
        AI.shared.obstacles = Arena.OBSTACLES;
        AI.shared.byId = state.byId;
        AI.shared.simT = state.elapsed;
        var teams = [[], []];
        for (var i = 0; i < state.agents.length; i++) {
            var a = state.agents[i];
            if (!a.unit.alive) continue;
            teams[a.unit.teamId].push(a);
        }
        AI.shared.teams = teams;
        AI.shared.teamFocus[0] = AI.chooseTeamFocus(teams[0], teams[1], Arena.OBSTACLES);
        AI.shared.teamFocus[1] = AI.chooseTeamFocus(teams[1], teams[0], Arena.OBSTACLES);
    };
})();
