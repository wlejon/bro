// agents/tactical.js — Proactive peek-shoot agent with coordinated focus
// fire. Delegates flee/heal to scripted when wounded; owns all engagement
// decisions otherwise.
//
// Pipeline (runs at PLAN_HZ per team):
//   1. TARGET ALLOCATION — Sort alive enemies by HP ascending (secure
//      kill-chain on the weakest). Greedy assign up to MAX_CONCENTRATION
//      blues per target by closest-LOS. Unassigned blues spill to the
//      weakest enemy.
//   2. FIRE/COVER PAIR SEARCH — For each blue, probe a grid of walkable
//      cells around the blue→target midpoint:
//        FIRE cell: LOS to target, within 0.55*range..0.95*range,
//                   minimise LOS exposure to OTHER enemies.
//        COVER cell: walkable, <2.6m from fire cell, NO LOS to target.
//      A cell pair only commits if BOTH exist; otherwise the agent falls
//      back to scripted baseline for that tick.
//
// Per-agent think (every 30 Hz scene AI tick):
//   - Aim continuously at the assigned target (aim lead happens naturally
//     while moving between cover and fire cells).
//   - State machine: COVER ─(shot ready)→ PEEK ─(fired|timeout)→ COVER.
//   - Fires on first tick that fire-cell is reached and BotAim cone lines
//     up with target, then immediately reverses to cover while the basic
//     cooldown burns.
//   - Opportunistic ability casts (heal-self, heal-ally, grenade/beam on
//     focus target) use scripted's mana-discipline gates applied here
//     directly — the tactical agent keeps the strategic ability logic
//     rather than delegating.
//
// Showcases from brogameagent:
//   - hasLineOfSight: lots — fire/cover scoring + per-tick LOS gates.
//   - NavGrid.isWalkable: every candidate cell is walkability-tested.
//   - Obstacles (AABB list) used as cover geometry implicitly — a cover
//     cell is simply "a cell where LOS to target is blocked", which is
//     only true when an obstacle sits between them.
//   - A* pathfinding (via agent.setTarget) handles the cover→fire hop.
(function () {
    "use strict";

    // ──── Tuning ────────────────────────────────────────────────────────
    var PLAN_HZ           = 4;            // re-search fire/cover @ 4 Hz (target moves fast)
    var PLAN_INTERVAL     = 1 / PLAN_HZ;
    var MAX_CONCENTRATION = 3;            // blues per focus target
    var PEEK_HP_FLOOR     = 0.35;         // below → fall back to scripted flee
    var PEEK_TIMEOUT      = 0.7;          // max seconds in PEEK before forcing retreat
    var REACHED_DIST      = 0.7;          // cell "reached" threshold

    // Fire-cell search grid (around blue→target midpoint, clamped near blue).
    var SEARCH_RADIUS     = 4.0;
    var SEARCH_STEP       = 1.0;          // 9x9 = 81 cells per blue
    var SEARCH_SHIFT_MAX  = 3.0;          // how far toward target to shift center
    // Cover-cell ring (around the fire cell).
    var COVER_MIN_R       = 0.8;
    var COVER_MAX_R       = 2.6;
    var COVER_ANGLES      = 12;
    // Spread control — blues assigned to the same target avoid cells
    // already claimed by a teammate. Two rules:
    //   - hard exclusion inside GRENADE_SPLASH (2.5m) — any closer and
    //     one grenade splashes multiple blues
    //   - soft nudge up to CLAIM_RADIUS with a small weight, so the
    //     spread doesn't push blues into poorly-exposed cells
    var GRENADE_SPLASH    = 2.5;
    var CLAIM_RADIUS      = 4.0;
    var CLAIM_WEIGHT      = 3;

    var PSPEED            = 18;           // basic projectile speed (matches ai.js)

    // ──── Shared helpers ───────────────────────────────────────────────
    function d2(ax, az, bx, bz) { var dx=ax-bx, dz=az-bz; return dx*dx + dz*dz; }
    function dist(ax, az, bx, bz) { return Math.sqrt(d2(ax, az, bx, bz)); }
    function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }
    function hasLOS(ax, az, bx, bz) {
        return bro.ai.game.hasLineOfSight(ax, az, bx, bz, AI.shared.obstacles);
    }

    // ──── Per-agent / per-team scratch ─────────────────────────────────
    var mem = {};
    function getTMem(id) {
        var m = mem[id];
        if (!m) m = mem[id] = {
            assignedTargetId: -1,
            fire: null, cover: null,
            peekState: "cover",
            peekTimer: 0,
            lastPlanT: -99,
            lastThinkT: -1,
            aim: BotAim.create({ turnSpeed: 5.0, sampleHz: 15, fireConeRad: 0.15 }),
            shootCd: 0,
            abCd: [0, 0, 0, 0, 0, 0, 0, 0],
            lastMoveX: null, lastMoveZ: null,
            intent: "tactical",
        };
        return m;
    }
    var teamStats = {};


    // ──── Target allocation ────────────────────────────────────────────
    // Priority = lowest HP first. Up to MAX_CONCENTRATION blues per
    // target, preferring blues that already have LOS and are close.
    // Unassigned blues fall through to the weakest enemy (overload).
    function allocateTargets(team, enemies) {
        var priorities = [];
        for (var i = 0; i < enemies.length; i++) {
            if (enemies[i].unit.alive) priorities.push(enemies[i]);
        }
        priorities.sort(function (a, b) { return a.unit.hp - b.unit.hp; });

        var assign = {};
        var counts = {};

        for (var p = 0; p < priorities.length; p++) {
            var e = priorities[p];
            if ((counts[e.unit.id] || 0) >= MAX_CONCENTRATION) continue;
            // Gather candidate blues with LOS, sorted by distance.
            var cands = [];
            for (var b = 0; b < team.length; b++) {
                var blue = team[b];
                if (!blue.unit.alive) continue;
                if (assign[blue.unit.id] !== undefined) continue;
                if (!hasLOS(blue.x, blue.z, e.x, e.z)) continue;
                cands.push({ b: blue, d: d2(blue.x, blue.z, e.x, e.z) });
            }
            cands.sort(function (a, b) { return a.d - b.d; });
            var need = MAX_CONCENTRATION - (counts[e.unit.id] || 0);
            for (var c = 0; c < Math.min(need, cands.length); c++) {
                assign[cands[c].b.unit.id] = e.unit.id;
                counts[e.unit.id] = (counts[e.unit.id] || 0) + 1;
            }
        }
        // Spill — unassigned blues get the weakest live enemy. Keeping
        // the team converged on the same priority target is the whole
        // point of focus fire; scattering to each blue's own weakest
        // visible enemy defeats that.
        if (priorities.length > 0) {
            var weakest = priorities[0].unit.id;
            for (var t = 0; t < team.length; t++) {
                var blue2 = team[t];
                if (!blue2.unit.alive) continue;
                if (assign[blue2.unit.id] === undefined) assign[blue2.unit.id] = weakest;
            }
        }
        return assign;
    }

    // ──── Fire / cover pair search ─────────────────────────────────────
    // Returns true if the cell is within a teammate's grenade-splash —
    // a hard exclusion: two blues this close share damage from any AoE.
    function splashConflict(x, z, claimed) {
        if (!claimed) return false;
        var r2 = GRENADE_SPLASH * GRENADE_SPLASH;
        for (var i = 0; i < claimed.length; i++) {
            var cx = claimed[i].x, cz = claimed[i].z;
            if ((x - cx) * (x - cx) + (z - cz) * (z - cz) < r2) return true;
        }
        return false;
    }
    // Soft penalty beyond splash — nudges spread across available cells
    // without overriding exposure/distance concerns.
    function claimPenalty(x, z, claimed) {
        if (!claimed || claimed.length === 0) return 0;
        var pen = 0;
        for (var i = 0; i < claimed.length; i++) {
            var cx = claimed[i].x, cz = claimed[i].z;
            var d = Math.sqrt((x - cx) * (x - cx) + (z - cz) * (z - cz));
            if (d >= GRENADE_SPLASH && d < CLAIM_RADIUS) {
                pen += (CLAIM_RADIUS - d) * CLAIM_WEIGHT / (CLAIM_RADIUS - GRENADE_SPLASH);
            }
        }
        return pen;
    }

    function findFireCoverPair(blue, target, otherEnemies, nav, claimedFire, claimedCover) {
        var bx = blue.x, bz = blue.z;
        var tx = target.x, tz = target.z;
        var range = blue.unit.attackRange || 9;
        var minR2 = (range * 0.55) * (range * 0.55);
        var maxR2 = (range * 0.95) * (range * 0.95);

        // Center the search on a point part-way toward the target so
        // blue's candidate cells are in a useful engage zone, not
        // centered on wherever they currently stand.
        var cx0 = (bx + tx) * 0.5;
        var cz0 = (bz + tz) * 0.5;
        var toMid = dist(bx, bz, cx0, cz0);
        if (toMid > SEARCH_SHIFT_MAX) {
            var sh = SEARCH_SHIFT_MAX / toMid;
            cx0 = bx + (cx0 - bx) * sh;
            cz0 = bz + (cz0 - bz) * sh;
        }

        var bestFire = null, bestFireScore = -Infinity;
        for (var dx = -SEARCH_RADIUS; dx <= SEARCH_RADIUS + 1e-6; dx += SEARCH_STEP) {
            for (var dz = -SEARCH_RADIUS; dz <= SEARCH_RADIUS + 1e-6; dz += SEARCH_STEP) {
                var fx = clamp(cx0 + dx, -19, 19);
                var fz = clamp(cz0 + dz, -19, 19);
                if (!nav.isWalkable(fx, fz)) continue;
                var ddT = d2(fx, fz, tx, tz);
                if (ddT < minR2 || ddT > maxR2) continue;
                if (!hasLOS(fx, fz, tx, tz)) continue;

                // Count exposures to OTHER enemies — cover is worthless
                // if another enemy has a free shot from this cell.
                var exposures = 0;
                for (var e = 0; e < otherEnemies.length; e++) {
                    var oe = otherEnemies[e];
                    if (!oe.unit.alive) continue;
                    if (d2(fx, fz, oe.x, oe.z) > 196) continue;  // 14m cutoff
                    if (hasLOS(fx, fz, oe.x, oe.z)) exposures++;
                }

                if (splashConflict(fx, fz, claimedFire)) continue;
                var score = -exposures * 4 - dist(fx, fz, bx, bz) * 0.3
                          - claimPenalty(fx, fz, claimedFire);
                if (score > bestFireScore) {
                    bestFireScore = score;
                    bestFire = { x: fx, z: fz };
                }
            }
        }
        if (!bestFire) return null;

        // Cover cell — must break LOS to target AND keep other-enemy
        // exposure ≤ 1. A cover spot facing the target but visible to
        // the rest of red's team gets shot anyway. Prefer the one with
        // fewest total exposures; tie-break by distance to fire cell
        // (short jiggle hops).
        var bestCover = null, bestCoverScore = Infinity;
        for (var r = COVER_MIN_R; r <= COVER_MAX_R + 1e-6; r += 0.5) {
            for (var a = 0; a < COVER_ANGLES; a++) {
                var theta = a * (Math.PI * 2) / COVER_ANGLES;
                var cx2 = clamp(bestFire.x + Math.cos(theta) * r, -19, 19);
                var cz2 = clamp(bestFire.z + Math.sin(theta) * r, -19, 19);
                if (!nav.isWalkable(cx2, cz2)) continue;
                if (hasLOS(cx2, cz2, tx, tz)) continue;
                var exp = 0;
                for (var e2 = 0; e2 < otherEnemies.length; e2++) {
                    var oe2 = otherEnemies[e2];
                    if (!oe2.unit.alive) continue;
                    if (d2(cx2, cz2, oe2.x, oe2.z) > 196) continue;
                    if (hasLOS(cx2, cz2, oe2.x, oe2.z)) exp++;
                }
                if (exp > 1) continue;  // reject cover cells exposed to multiple enemies
                if (splashConflict(cx2, cz2, claimedCover)) continue;
                var dc = d2(cx2, cz2, bestFire.x, bestFire.z);
                var cscore = exp * 100 + dc + claimPenalty(cx2, cz2, claimedCover);
                if (cscore < bestCoverScore) {
                    bestCoverScore = cscore;
                    bestCover = { x: cx2, z: cz2 };
                }
            }
        }
        if (!bestCover) return null;
        return { fire: bestFire, cover: bestCover };
    }

    // ──── Abilities (simple mana-discipline, uses scripted thresholds) ─
    function tryAbilities(agent, m, target, teammates, enemies, world) {
        var u = agent.unit;
        // HEAL — self below 55% or ally below 55% within 4m.
        if (u.mana >= 25 && m.abCd[Arena.AB_HEAL] <= 0) {
            var hpFrac = u.hp / u.maxHp;
            if (hpFrac < 0.55) {
                if (world.resolveAbility(agent, Arena.AB_HEAL, u.id)) {
                    m.abCd[Arena.AB_HEAL] = 6; return true;
                }
            }
            var wounded = null, wHp = 0.55;
            for (var i = 0; i < teammates.length; i++) {
                var a = teammates[i];
                if (a === agent || !a.unit.alive) continue;
                if (d2(a.x, a.z, agent.x, agent.z) > 16) continue;
                var f = a.unit.hp / a.unit.maxHp;
                if (f < wHp) { wHp = f; wounded = a; }
            }
            if (wounded && world.resolveAbility(agent, Arena.AB_HEAL, wounded.unit.id)) {
                m.abCd[Arena.AB_HEAL] = 6; return true;
            }
        }
        if (!target || !target.unit.alive) return false;
        var tDx = target.x - agent.x, tDz = target.z - agent.z;
        var tD = Math.sqrt(tDx*tDx + tDz*tDz);
        if (!hasLOS(agent.x, agent.z, target.x, target.z)) return false;

        // GRENADE — cluster ≥ 2 near target, no friendly inside splash.
        if (u.mana >= 35 && tD > 2 && tD < 12 && m.abCd[Arena.AB_GRENADE] <= 0) {
            var cluster = 0, friendlyHit = false;
            for (var e = 0; e < enemies.length; e++) {
                if (!enemies[e].unit.alive) continue;
                if (d2(enemies[e].x, enemies[e].z, target.x, target.z) < 6.25) cluster++;
            }
            for (var t = 0; t < teammates.length; t++) {
                if (d2(teammates[t].x, teammates[t].z, target.x, target.z) < 6.25) {
                    friendlyHit = true; break;
                }
            }
            if (cluster >= 2 && !friendlyHit
                && world.resolveAbility(agent, Arena.AB_GRENADE, target.unit.id)) {
                m.abCd[Arena.AB_GRENADE] = 5; return true;
            }
        }
        // BEAM — fire when target is low or collinear pierce opportunity.
        if (u.mana >= 30 && tD > 2 && tD < 16 && m.abCd[Arena.AB_BEAM] <= 0) {
            if (target.unit.hp < 30
                && world.resolveAbility(agent, Arena.AB_BEAM, target.unit.id)) {
                m.abCd[Arena.AB_BEAM] = 3.5; return true;
            }
        }
        return false;
    }

    // ──── Basic shot ───────────────────────────────────────────────────
    function tryFire(agent, m, target, world) {
        if (m.shootCd > 0) return false;
        if (!target || !target.unit.alive) return false;
        var u = agent.unit;
        if (dist(agent.x, agent.z, target.x, target.z) > u.attackRange) return false;
        if (!hasLOS(agent.x, agent.z, target.x, target.z)) return false;
        if (!BotAim.canFireAt(m.aim, agent.x, 0, agent.z, target.x, 0, target.z)) return false;

        var f = BotAim.forward(m.aim);
        world.spawnProjectile({
            ownerId: u.id, teamId: u.teamId,
            x: agent.x + f.x * (u.radius + 0.4),
            z: agent.z + f.z * (u.radius + 0.4),
            vx: f.x * PSPEED, vz: f.z * PSPEED,
            speed: PSPEED, radius: 0.22,
            damage: 9, remainingLife: 1.2,
            kind: "physical", mode: "single",
        });
        m.shootCd = 1.0 / Math.max(0.1, u.attacksPerSec);
        return true;
    }

    // ──── Team tick: allocate + search per PLAN_INTERVAL ───────────────
    function teamTick(state, teamId /*, dt*/) {
        var now = state.elapsed;
        var teams = AI.shared.teams;
        var team = teams[teamId];
        var enemies = teams[1 - teamId];
        if (team.length === 0 || enemies.length === 0) return;
        var nav = state.nav;

        var assign = allocateTargets(team, enemies);

        // Build per-blue work list, sorted by distance to assigned target
        // (closest blue picks first — gets the best cell — subsequent
        // blues are pushed to spread positions by claim penalty).
        var work = [];
        for (var i = 0; i < team.length; i++) {
            var blue = team[i];
            if (!blue.unit.alive) continue;
            var m = getTMem(blue.unit.id);
            var tid = assign[blue.unit.id];
            m.assignedTargetId = (tid === undefined) ? -1 : tid;

            if (m.pairForTarget !== m.assignedTargetId) {
                m.fire = null; m.cover = null;
                m.pairForTarget = m.assignedTargetId;
                m.lastPlanT = -99;
            }
            if (now - m.lastPlanT < PLAN_INTERVAL) continue;
            if (m.assignedTargetId < 0) continue;

            var target = null;
            for (var j = 0; j < enemies.length; j++) {
                if (enemies[j].unit.id === m.assignedTargetId) { target = enemies[j]; break; }
            }
            if (!target || !target.unit.alive) {
                m.fire = null; m.cover = null;
                m.lastPlanT = now;
                continue;
            }
            work.push({
                blue: blue, m: m, target: target,
                d: d2(blue.x, blue.z, target.x, target.z),
            });
        }
        work.sort(function (a, b) { return a.d - b.d; });

        // Claimed cells per target — every blue planning against target T
        // sees the fire/cover cells chosen by earlier-planned blues also
        // assigned to T, and is penalised for clustering on them.
        var claimsByTarget = {};
        var pairsFound = 0;
        for (var w = 0; w < work.length; w++) {
            var W = work[w];
            var tid2 = W.m.assignedTargetId;
            var claims = claimsByTarget[tid2] ||
                         (claimsByTarget[tid2] = { fire: [], cover: [] });
            var others = [];
            for (var k = 0; k < enemies.length; k++) {
                if (enemies[k] !== W.target && enemies[k].unit.alive) others.push(enemies[k]);
            }
            var pair = findFireCoverPair(W.blue, W.target, others, nav,
                                         claims.fire, claims.cover);
            if (pair) {
                W.m.fire = pair.fire;
                W.m.cover = pair.cover;
                claims.fire.push(pair.fire);
                claims.cover.push(pair.cover);
                pairsFound++;
            } else {
                W.m.fire = null;
                W.m.cover = null;
            }
            W.m.lastPlanT = now;
        }

        teamStats[teamId] = {
            label: "tactical [team " + (teamId === 1 ? "blue" : "red") + "]",
            "assigned": Object.keys(assign).length,
            "pairs":    pairsFound,
        };
    }

    // Deduped movement — A* replan thrash otherwise.
    function moveTo(self, m, x, z) {
        var tx = Math.round(x * 2) * 0.5;
        var tz = Math.round(z * 2) * 0.5;
        if (tx === m.lastMoveX && tz === m.lastMoveZ) return;
        m.lastMoveX = tx; m.lastMoveZ = tz;
        self.moveTo(tx, tz);
    }

    // ──── Per-agent think ──────────────────────────────────────────────
    function tacticalThink(self, world) {
        var agent = self.agent;
        var u = agent.unit;
        if (!u.alive) { self.hold(0.5); return; }

        var m = getTMem(u.id);
        var simT = AI.shared.simT;
        var prevT = m.lastThinkT < 0 ? simT : m.lastThinkT;
        var dt = Math.max(0.001, Math.min(0.2, simT - prevT));
        m.lastThinkT = simT;
        if (m.shootCd > 0) m.shootCd -= dt;
        for (var cd = 0; cd < m.abCd.length; cd++) {
            if (m.abCd[cd] > 0) m.abCd[cd] -= dt;
        }
        m.peekTimer += dt;

        var hpFrac = u.hp / u.maxHp;
        // Low HP → scripted's flee/rally path is already well-tuned.
        if (hpFrac < PEEK_HP_FLOOR) {
            m.lastMoveX = null; m.lastMoveZ = null;
            Agents.get("scripted").think(self, world);
            return;
        }

        // Lookup assigned target.
        var target = null;
        if (m.assignedTargetId >= 0 && AI.shared.byId) {
            target = AI.shared.byId[m.assignedTargetId] || null;
            if (target && !target.unit.alive) target = null;
        }
        if (!target || !m.fire || !m.cover) {
            m.lastMoveX = null; m.lastMoveZ = null;
            Agents.get("scripted").think(self, world);
            return;
        }
        // Fire cell LOS might be stale if target moved since planning.
        // If we can't actually shoot the target from the committed cell,
        // bail to scripted — which will close distance and let the next
        // plan tick find a better cell.
        if (!hasLOS(m.fire.x, m.fire.z, target.x, target.z)) {
            m.lastMoveX = null; m.lastMoveZ = null;
            Agents.get("scripted").think(self, world);
            return;
        }

        var teammates = AI.shared.teams[u.teamId];
        var enemies = AI.shared.teams[1 - u.teamId];

        // Continuous aim at target center.
        var yaw = Math.atan2(target.x - agent.x, -(target.z - agent.z));
        BotAim.requestAim(m.aim, simT, yaw, 0);
        BotAim.tick(m.aim, dt);

        // Opportunistic ability cast (heal self, grenade cluster, beam
        // finisher) — gated by scripted's mana thresholds.
        tryAbilities(agent, m, target, teammates, enemies, world);

        // Opportunistic fire — tryFire gates on cooldown + range + LOS +
        // aim cone. Fires from wherever blue is if the shot is there.
        tryFire(agent, m, target, world);

        // Movement: when the shot is reloading, fall back to the cover
        // cell (out of LOS to target); when ready again, advance to the
        // fire cell. Staying at the current spot when we have LOS but
        // the aim cone hasn't caught up beats bouncing to a cell we'll
        // arrive at with the cooldown half-burned.
        var dest;
        if (m.shootCd > 0) {
            dest = m.cover;
        } else {
            var hasLosNow = hasLOS(agent.x, agent.z, target.x, target.z);
            var rSq = u.attackRange * u.attackRange;
            if (hasLosNow && d2(agent.x, agent.z, target.x, target.z) <= rSq) {
                dest = { x: agent.x, z: agent.z };
            } else {
                dest = m.fire;
            }
        }
        moveTo(self, m, dest.x, dest.z);
    }

    Agents.register({
        id: "tactical",
        label: "Tactical (peek + focus)",
        reset: function () { mem = {}; teamStats = {}; },
        teamTick: teamTick,
        think: tacticalThink,
        stats: function (state, teamId) { return teamStats[teamId] || null; },
    });
})();
