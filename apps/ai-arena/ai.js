// ai.js — Bot policies (tactical scripted + MCTS).
//
// Designed to show visible intelligence, not just "walk to nearest enemy":
//   - team focus-fire on weakest visible enemy
//   - retreat + heal on low HP
//   - kiting: hold ideal attack distance instead of hugging
//   - ability use keyed on situation (cluster → grenade, line-up → beam)
//   - LOS-aware: reposition when target is behind cover
var AI = {};
(function () {
    "use strict";

    function dist2(ax, az, bx, bz) {
        var dx = ax - bx, dz = az - bz;
        return dx * dx + dz * dz;
    }
    function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

    // Per-agent persistent state — remembered across ticks.
    AI.memory = {};
    AI.getMem = function (id) {
        var m = AI.memory[id];
        if (!m) m = AI.memory[id] = {
            intent: "idle", perpSign: 1, lastFlip: -99,
            shootCd: 0, targetId: null,
            // Aim direction — separate from movement yaw so render (and
            // future gun-based fire) can show where we're actually looking.
            aimX: 1, aimZ: 0,
            // Threat tracking — recent damage taken & who dealt most of it.
            threat: 0, threatSourceId: -1, lastHitT: -99,
            role: "front", coverX: null, coverZ: null,
            fleeX: null, fleeZ: null, fleePickedT: -99,
        };
        return m;
    };

    // Decay threat pressure; called once per agent per tick.
    AI.decayThreat = function (mem, dt) {
        var decay = Math.exp(-dt / 1.5);  // half-life ~1s
        mem.threat *= decay;
        if (mem.threat < 0.5) mem.threatSourceId = -1;
    };

    // Called from main.js when a damage event is drained from world.events.
    AI.recordDamage = function (targetId, attackerId, amount, simT) {
        var m = AI.getMem(targetId);
        m.threat += amount;
        m.lastHitT = simT;
        // Prefer sticking with the same threat source unless a new attacker
        // is hitting us harder — this prevents thrashing between two shooters.
        if (attackerId !== m.threatSourceId) {
            if (m.threatSourceId < 0 || amount * 2 > m.threat) {
                m.threatSourceId = attackerId;
            }
        }
    };

    // Fire a projectile at `focus` if cooldown + LOS allow. Updates mem.shootCd.
    // Uses computeLeadAim when the focus is moving fast enough to matter.
    AI.tryShoot = function (agent, world, focus, obstacles, mem, dt) {
        mem.shootCd -= dt;
        if (!focus || !focus.unit.alive) return false;
        if (mem.shootCd > 0) return false;
        var u = agent.unit;
        var dx = focus.x - agent.x, dz = focus.z - agent.z;
        var dist = Math.sqrt(dx * dx + dz * dz);
        if (dist > u.attackRange) return false;
        if (!bro.ai.game.hasLineOfSight(agent.x, agent.z, focus.x, focus.z, obstacles)) {
            return false;
        }

        var PSPEED = 18;
        // Lead the target if it's moving meaningfully.
        var fv = focus.velocity;
        var vMag = Math.sqrt(fv.x * fv.x + fv.z * fv.z);
        var vx = dx, vz = dz;
        if (vMag > 0.5) {
            var lead = bro.ai.game.computeLeadAim(
                agent.x, 1, agent.z,
                focus.x, 1, focus.z,
                fv.x, 0, fv.z,
                PSPEED);
            if (lead.valid) {
                // computeLeadAim returns yaw/pitch using -Z forward.
                // Convert to velocity vector: vx=sin(yaw), vz=-cos(yaw).
                vx = Math.sin(lead.yaw);
                vz = -Math.cos(lead.yaw);
            }
        }
        var mag = Math.max(0.01, Math.sqrt(vx * vx + vz * vz));
        world.spawnProjectile({
            ownerId: u.id,
            teamId:  u.teamId,
            x: agent.x + (dx / Math.max(0.01, dist)) * (u.radius + 0.4),
            z: agent.z + (dz / Math.max(0.01, dist)) * (u.radius + 0.4),
            vx: (vx / mag) * PSPEED,
            vz: (vz / mag) * PSPEED,
            speed: PSPEED,
            radius: 0.22,
            damage: 9,
            remainingLife: 1.2,
            kind: "physical",
            mode: "single",
        });
        mem.shootCd = 1.0 / Math.max(0.1, u.attacksPerSec);
        return true;
    };

    // Choose a team focus target = weakest enemy that at least one teammate can see.
    // Falls back to the overall closest enemy if nobody has LOS.
    //
    // Perception is distance-capped at ~1.8x attackRange so the team doesn't
    // commit to a target they can see but can't shoot — prevents the "march
    // across the arena in a straight line into a killzone" behavior.
    AI.chooseTeamFocus = function (team, enemies, obstacles) {
        var best = null, bestHp = Infinity;
        var PERCEPT_MUL = 1.8;
        for (var i = 0; i < enemies.length; i++) {
            var e = enemies[i];
            if (!e.unit.alive) continue;
            var seen = false;
            for (var j = 0; j < team.length; j++) {
                var t = team[j];
                if (!t.unit.alive) continue;
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
        // Fallback: nearest to any teammate (no LOS required)
        var bestD = Infinity;
        for (var k = 0; k < enemies.length; k++) {
            var en = enemies[k];
            if (!en.unit.alive) continue;
            for (var m = 0; m < team.length; m++) {
                var tm = team[m];
                if (!tm.unit.alive) continue;
                var d = dist2(tm.x, tm.z, en.x, en.z);
                if (d < bestD) { bestD = d; best = en; }
            }
        }
        return best;
    };

    // Per-agent target selection: closest visible enemy, weighted toward low HP.
    // Falls back to the team-level focus if nothing is in line of sight.
    AI.pickTargetFor = function (agent, enemies, teamFocus, obstacles) {
        var best = null, bestScore = -Infinity;
        var ax = agent.x, az = agent.z;
        for (var i = 0; i < enemies.length; i++) {
            var e = enemies[i];
            if (!e.unit.alive) continue;
            var dx = e.x - ax, dz = e.z - az;
            var d = Math.sqrt(dx * dx + dz * dz);
            if (d > 18) continue;  // too far to bother
            if (!bro.ai.game.hasLineOfSight(ax, az, e.x, e.z, obstacles)) continue;
            // Prefer close, low-HP targets. Distance dominates unless HP is tiny.
            var score = -d - e.unit.hp * 0.04;
            if (score > bestScore) { bestScore = score; best = e; }
        }
        return best || teamFocus;
    };

    // Pick a walkable cover point. A cell is "cover" relative to threats if
    // the straight LOS line from every listed threat to that cell is blocked
    // by an obstacle. We prefer cells that are (a) close to the agent,
    // (b) toward the team centroid (don't break ranks), (c) not directly
    // backward (keeps pressure on enemies). Returns {x, z} or null.
    //
    //  agent      — the one seeking cover (for current-position distance penalty)
    //  threats    — array of {x, z} positions (enemies currently hitting us)
    //  obstacles  — Arena.OBSTACLES
    //  nav        — nav grid
    //  opts       — { anchorX, anchorZ, anchorRadius, claimed } — optional:
    //    anchor*  : sample ring is centered here instead of `agent` (for
    //               rallying near a healthy ally without landing on top of
    //               them); agent position is still used as the travel-cost
    //               penalty so the returned point is reachable.
    //    claimed  : array of {x, z} points already chosen by other agents
    //               this tick. Candidates within 1u of any claimed point
    //               are rejected so wounded ralliers fan out instead of
    //               piling onto the same cell.
    //    minThreatDistance : reject candidates closer than this to any
    //               threat. Use when a low-HP unit needs to heal safely —
    //               LOS blocked isn't enough if the threat can reach us.
    AI.findCover = function (agent, threats, obstacles, nav, opts) {
        if (!threats || threats.length === 0) return null;
        opts = opts || {};
        var ax = agent.x, az = agent.z;
        var cx0 = opts.anchorX !== undefined ? opts.anchorX : ax;
        var cz0 = opts.anchorZ !== undefined ? opts.anchorZ : az;
        // Wider search if anchored to an ally — the agent may need to
        // traverse further to reach useful cover near them.
        var rings = opts.anchorX !== undefined
            ? [1.5, 2.5, 3.5, 5.0]
            : [2.0, 3.5, 5.0];
        var claimed = opts.claimed || null;

        var best = null, bestScore = -Infinity;
        for (var ri = 0; ri < rings.length; ri++) {
            var r = rings[ri];
            for (var ang = 0; ang < Math.PI * 2; ang += Math.PI / 6) {
                var cx = clamp(cx0 + Math.cos(ang) * r, -19, 19);
                var cz = clamp(cz0 + Math.sin(ang) * r, -19, 19);
                if (!nav.isWalkable(cx, cz)) continue;

                // Reject a candidate that's effectively on top of another
                // rallier's chosen cover — spreads wounded units around
                // instead of clumping them.
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

                // Must break LOS to EVERY known threat — half-cover (blocked
                // from one but exposed to another) is a trap.
                var coversAll = true;
                var tooCloseToThreat = false;
                var minThD2 = opts.minThreatDistance
                    ? opts.minThreatDistance * opts.minThreatDistance
                    : 0;
                for (var ti = 0; ti < threats.length; ti++) {
                    var th = threats[ti];
                    if (minThD2 > 0) {
                        var tdx = cx - th.x, tdz = cz - th.z;
                        if (tdx * tdx + tdz * tdz < minThD2) {
                            tooCloseToThreat = true; break;
                        }
                    }
                    if (bro.ai.game.hasLineOfSight(cx, cz, th.x, th.z, obstacles)) {
                        coversAll = false; break;
                    }
                }
                if (tooCloseToThreat || !coversAll) continue;

                // Score: short travel from agent dominates; mild pull toward
                // the anchor point (so ally-anchored searches prefer cells
                // near that ally without ignoring reachability entirely).
                var score = -Math.hypot(cx - ax, cz - az);
                if (opts.anchorX !== undefined) {
                    score -= 0.4 * Math.hypot(cx - cx0, cz - cz0);
                }
                if (score > bestScore) { bestScore = score; best = { x: cx, z: cz }; }
            }
        }
        return best;
    };

    // Per-tick list of cover points already claimed by ralliers this frame.
    // main.js resets this at the top of each sim step; scriptedTactical
    // appends its picks so downstream agents fan out.
    AI.claimedCover = [];
    AI.resetClaimedCover = function () { AI.claimedCover.length = 0; };

    // Push a candidate move destination away from close teammates so the
    // firing line doesn't collapse into a single blob. Returns the adjusted
    // {x, z}. The nudge scales with how close teammates are — harmless at
    // comfortable spacing, strong when stacked.
    AI.spaceFromTeammates = function (agent, teammates, x, z) {
        var SPACING = 1.4;
        for (var ti = 0; ti < teammates.length; ti++) {
            var tm = teammates[ti];
            if (tm === agent || !tm.unit.alive) continue;
            var ddx = x - tm.x, ddz = z - tm.z;
            var dd = Math.hypot(ddx, ddz);
            if (dd < SPACING && dd > 0.01) {
                var push = (SPACING - dd);
                x += (ddx / dd) * push;
                z += (ddz / dd) * push;
            }
        }
        return { x: clamp(x, -19, 19), z: clamp(z, -19, 19) };
    };

    // Push a point out of enemy attack range along the net "away from
    // enemies" vector. Used by the flee branch so wounded units heal from
    // a position the enemy can't reach, not from two steps inside the
    // firing line. Iterates a few times since pushing away from one
    // enemy can bring us closer to another.
    AI.pushOutOfEnemyRange = function (x, z, enemies, slack) {
        slack = slack || 1.15;
        for (var iter = 0; iter < 4; iter++) {
            var worst = null, worstExcess = 0;
            for (var ei = 0; ei < enemies.length; ei++) {
                var e = enemies[ei];
                if (!e.unit.alive) continue;
                var safeR = (e.unit.attackRange || 9) * slack;
                var dx = x - e.x, dz = z - e.z;
                var d = Math.hypot(dx, dz);
                if (d >= safeR) continue;
                var excess = safeR - d;
                if (excess > worstExcess) {
                    worstExcess = excess;
                    worst = { dx: dx, dz: dz, d: d, push: excess };
                }
            }
            if (!worst) return { x: clamp(x, -19, 19), z: clamp(z, -19, 19) };
            var d2 = Math.max(0.01, worst.d);
            x += (worst.dx / d2) * worst.push;
            z += (worst.dz / d2) * worst.push;
            x = clamp(x, -19, 19);
            z = clamp(z, -19, 19);
        }
        return { x: x, z: z };
    };

    // Find a walkable point near (x, z), sampling outward in a ring. Returns
    // null if no walkable cell found within the search radius.
    AI.findWalkableNear = function (nav, x, z, maxR) {
        if (nav.isWalkable(x, z)) return { x: x, z: z };
        var STEP = 0.5;
        for (var r = STEP; r <= (maxR || 4); r += STEP) {
            for (var ang = 0; ang < Math.PI * 2; ang += Math.PI / 4) {
                var nx = x + Math.cos(ang) * r;
                var nz = z + Math.sin(ang) * r;
                if (nav.isWalkable(nx, nz)) return { x: nx, z: nz };
            }
        }
        return null;
    };

    // Decide the action for one agent given the team's current focus target.
    // Returns { useAbilityId, abilityTargetId, attackTargetId } + sets nav target.
    AI.scriptedTactical = function (agent, world, nav, enemies, teammates, focus, obstacles, simT) {
        var act = { useAbilityId: -1, abilityTargetId: -1, attackTargetId: -1 };
        var u = agent.unit;
        var mem = AI.getMem(u.id);

        // Unstick — if steering nudged us into a padded obstacle cell,
        // findPath from here returns []. setTarget alone won't help because
        // the pathfinder refuses to start on a blocked cell, so teleport
        // the agent to the nearest walkable neighbor (one-frame slide).
        if (nav && !nav.isWalkable(agent.x, agent.z)) {
            var free = AI.findWalkableNear(nav, agent.x, agent.z, 4);
            if (free) {
                agent.setPosition(free.x, free.z);
                mem.intent = "UNSTICK";
                // Fall through — re-plan normally on the next tick.
            }
        }

        // Nearest actual enemy for flee calculations
        var closest = null, closestD2 = Infinity;
        for (var i = 0; i < enemies.length; i++) {
            var en = enemies[i];
            if (!en.unit.alive) continue;
            var d2 = dist2(agent.x, agent.z, en.x, en.z);
            if (d2 < closestD2) { closestD2 = d2; closest = en; }
        }

        // If there are no live enemies at all, go idle — don't run the
        // flee/heal branch (that's the "zombie heal" stuck state).
        if (!closest) {
            mem.intent = "idle";
            mem.targetId = null;
            agent.clearTarget();
            return act;
        }

        // Clear the flee destination latch once HP has recovered, so
        // the next time we drop below threshold we pick a fresh spot
        // instead of using a stale cached one from two fights ago.
        if (u.hp / u.maxHp >= 0.45) {
            mem.fleeX = null;
            mem.fleeZ = null;
        }

        // ── FLEE + HEAL when wounded ──────────────────────────────────
        // Rally to the healthiest nearby ally while firing back at the
        // nearest visible threat. The old behavior ran to the corner and
        // stared at the wall, which was a great way to die alone.
        if (u.hp / u.maxHp < 0.35) {
            mem.intent = "FLEE";
            if (u.mana >= 25) {
                act.useAbilityId = Arena.AB_HEAL;
                act.abilityTargetId = u.id;
                mem.intent = "HEAL";
            }

            // Pick a rally point — prefer a healthy teammate that's also
            // ITSELF out of enemy range, so we don't run to an ally who's
            // in the middle of a firefight and get shot while healing.
            // Score: HP weighted heavily, distance penalty, big bonus for
            // "safe" allies (beyond enemy attack range of any living
            // enemy). Fall back to team centroid if no rally ally exists.
            var rally = null, rallyScore = -Infinity;
            var tcx = 0, tcz = 0, tcN = 0;
            for (var mi = 0; mi < teammates.length; mi++) {
                var mt = teammates[mi];
                if (!mt.unit.alive || mt === agent) continue;
                tcx += mt.x; tcz += mt.z; tcN++;
                var mHp = mt.unit.hp / mt.unit.maxHp;
                if (mHp < 0.5) continue;  // no point rallying to another casualty
                // Closest enemy to THIS candidate ally.
                var mateNearestEnemy = Infinity;
                for (var mei = 0; mei < enemies.length; mei++) {
                    var me2 = enemies[mei];
                    if (!me2.unit.alive) continue;
                    var mEd = Math.hypot(me2.x - mt.x, me2.z - mt.z);
                    if (mEd < mateNearestEnemy) mateNearestEnemy = mEd;
                }
                var safeR = (u.attackRange || 9) * 1.1;
                var safeBonus = (mateNearestEnemy > safeR) ? 60 : 0;
                var md = Math.hypot(mt.x - agent.x, mt.z - agent.z);
                var score = mHp * 100 - md + safeBonus;
                if (score > rallyScore) { rallyScore = score; rally = mt; }
            }
            // Latch the flee destination in memory. Recomputing every
            // tick — which we used to do — made the whole squad chase
            // the same moving "safe spot" and jitter on top of each
            // other. We only re-pick when:
            //   (a) nothing latched yet, OR
            //   (b) an enemy got too close to our latched spot, OR
            //   (c) the pick is stale (>3s), so we don't get stranded on
            //       the wrong side of a shifted battle line.
            var cachedStillGood = mem.fleeX !== null &&
                mem.fleeX !== undefined;
            if (cachedStillGood) {
                var enemyNearCached = false;
                for (var ci3 = 0; ci3 < enemies.length; ci3++) {
                    var en3 = enemies[ci3];
                    if (!en3.unit.alive) continue;
                    var dE = Math.hypot(en3.x - mem.fleeX, en3.z - mem.fleeZ);
                    if (dE < (en3.unit.attackRange || 9)) {
                        enemyNearCached = true; break;
                    }
                }
                if (enemyNearCached) cachedStillGood = false;
                if ((simT - (mem.fleePickedT || -99)) > 3.0) cachedStillGood = false;
            }

            if (cachedStillGood) {
                // Reuse the latched destination — no recomputation,
                // no jitter. Still claim the cell so the rest of the
                // team's picks avoid it.
                AI.claimedCover.push({ x: mem.fleeX, z: mem.fleeZ });
                // Fall straight through to the setTarget call below.
            }

            // Pick a rally destination. Priority:
            //  1. A cover cell near the healthy ally (ally shielding us,
            //     geometry blocking the shooter). This prevents the
            //     "rotating blob around one healthy unit" problem — we
            //     stop at a tactical standoff instead of colliding with
            //     the ally.
            //  2. A free spot offset from the ally opposite the threat.
            //  3. Team centroid (if no rally ally).
            //  4. Straight retreat away from the closest enemy.
            var fx = agent.x, fz = agent.z;
            var foundDest = false;

            if (cachedStillGood) {
                fx = mem.fleeX;
                fz = mem.fleeZ;
                foundDest = true;
            }

            if (!foundDest && rally) {
                // Push the anchor 1.8u past the ally away from the threat
                // — we want the ally BETWEEN us and the shooter, not
                // crowding the ally themselves.
                var thx = closest.x, thz = closest.z;
                var avx = rally.x - thx, avz = rally.z - thz;
                var avm = Math.max(0.01, Math.hypot(avx, avz));
                var anchorX = rally.x + (avx / avm) * 1.8;
                var anchorZ = rally.z + (avz / avm) * 1.8;
                var flCover = AI.findCover(agent, [closest], obstacles, nav, {
                    anchorX: anchorX, anchorZ: anchorZ,
                    claimed: AI.claimedCover,
                });
                if (flCover) {
                    fx = flCover.x; fz = flCover.z;
                    AI.claimedCover.push(flCover);
                    foundDest = true;
                }
            }

            if (!foundDest) {
                // No cover found — pick a fallback offset from the rally
                // anchor (or centroid, or reverse-from-threat for lone
                // survivors) that avoids stacking on teammates.
                var rx, rz;
                if (rally) {
                    // Offset opposite the threat, at a standoff distance.
                    var thx2 = closest.x, thz2 = closest.z;
                    var avx2 = rally.x - thx2, avz2 = rally.z - thz2;
                    var avm2 = Math.max(0.01, Math.hypot(avx2, avz2));
                    rx = rally.x + (avx2 / avm2) * 1.8;
                    rz = rally.z + (avz2 / avm2) * 1.8;
                } else if (tcN > 0) {
                    // All teammates are wounded too — there's no safe
                    // ally to rally to. Instead of everyone targeting
                    // the centroid (which is exactly where they already
                    // are, causing a stack), spread around it at an
                    // agent-id-derived angle. Golden-angle step gives
                    // well-distributed slots even with arbitrary ids.
                    var cx3 = tcx / tcN, cz3 = tcz / tcN;
                    var slotAng = (u.id * 2.39996) % (Math.PI * 2);
                    rx = cx3 + Math.cos(slotAng) * 2.5;
                    rz = cz3 + Math.sin(slotAng) * 2.5;
                } else {
                    rx = agent.x + (agent.x - closest.x);
                    rz = agent.z + (agent.z - closest.z);
                }
                // Nudge away from any already-claimed rally points so
                // the wounded unit behind us picks a different spot.
                for (var ci2 = 0; ci2 < AI.claimedCover.length; ci2++) {
                    var cl2 = AI.claimedCover[ci2];
                    var ddx = rx - cl2.x, ddz = rz - cl2.z;
                    var dd = Math.hypot(ddx, ddz);
                    if (dd < 1.2 && dd > 0.01) {
                        rx += (ddx / dd) * (1.2 - dd);
                        rz += (ddz / dd) * (1.2 - dd);
                    }
                }
                fx = clamp(rx, -19, 19);
                fz = clamp(rz, -19, 19);
                if (nav && !nav.isWalkable(fx, fz)) {
                    var fw = AI.findWalkableNear(nav, fx, fz, 3);
                    if (fw) { fx = fw.x; fz = fw.z; }
                }
                AI.claimedCover.push({ x: fx, z: fz });
            }
            if (!cachedStillGood) {
                // Final safety pass: shove the destination out of enemy
                // attack range. Cover might still be within range of a
                // second shooter we weren't tracking as the primary
                // threat — we'd rather take one extra step than heal
                // under fire.
                var safe = AI.pushOutOfEnemyRange(fx, fz, enemies, 1.15);
                if (nav && !nav.isWalkable(safe.x, safe.z)) {
                    var safeW = AI.findWalkableNear(nav, safe.x, safe.z, 3);
                    if (safeW) { fx = safeW.x; fz = safeW.z; }
                } else {
                    fx = safe.x; fz = safe.z;
                }
                // Space from teammates so N wounded units don't all pick
                // the same pushed-out point. Applied once at pick time;
                // the latch keeps it from drifting on subsequent ticks.
                var spaced = AI.spaceFromTeammates(agent, teammates, fx, fz);
                fx = spaced.x; fz = spaced.z;
                if (nav && !nav.isWalkable(fx, fz)) {
                    var sw2 = AI.findWalkableNear(nav, fx, fz, 3);
                    if (sw2) { fx = sw2.x; fz = sw2.z; }
                }
                // Latch for future ticks.
                mem.fleeX = fx;
                mem.fleeZ = fz;
                mem.fleePickedT = simT;
            }
            agent.setTarget(fx, fz);

            // Fire while retreating — pick the best shot we have right
            // now, prefer the primary threat, fall back to any visible
            // enemy in range. Aim latches to the shot target so we're
            // firing over-the-shoulder instead of facing our escape path.
            var shootAt = null;
            var range = u.attackRange;
            for (var ei = 0; ei < enemies.length; ei++) {
                var en = enemies[ei];
                if (!en.unit.alive) continue;
                var ed = Math.hypot(en.x - agent.x, en.z - agent.z);
                if (ed > range) continue;
                if (!bro.ai.game.hasLineOfSight(agent.x, agent.z, en.x, en.z, obstacles)) continue;
                if (en.unit.id === mem.threatSourceId) { shootAt = en; break; }
                if (!shootAt) shootAt = en;
            }
            if (shootAt) {
                act.fireAt = shootAt;
                var sdx = shootAt.x - agent.x, sdz = shootAt.z - agent.z;
                var smag = Math.max(0.01, Math.hypot(sdx, sdz));
                mem.aimX = sdx / smag;
                mem.aimZ = sdz / smag;
            }
            return act;
        }

        if (!focus || !focus.unit.alive) {
            mem.intent = "idle";
            mem.targetId = null;
            agent.clearTarget();
            return act;
        }
        mem.targetId = focus.unit.id;
        // Latch aim toward the current focus so render + gun mechanics
        // can use "where we're looking" independent of strafe direction.
        var adx = focus.x - agent.x, adz = focus.z - agent.z;
        var admag = Math.max(0.01, Math.hypot(adx, adz));
        mem.aimX = adx / admag;
        mem.aimZ = adz / admag;

        // ── ALLY HEAL ──────────────────────────────────────────────────
        // If a nearby teammate is hurt worse than us and we can cast heal,
        // prioritize them over our own self-heal. Range is 4u (matches
        // the registered ability range). We pick the most-wounded in range
        // so the heal goes where it matters most.
        if (u.mana >= 25) {
            var bestWoundedAlly = null, bestAllyHp = u.hp / u.maxHp;
            for (var ahi = 0; ahi < teammates.length; ahi++) {
                var at = teammates[ahi];
                if (!at.unit.alive || at === agent) continue;
                var ahd = Math.hypot(at.x - agent.x, at.z - agent.z);
                if (ahd > 4) continue;
                var ahp = at.unit.hp / at.unit.maxHp;
                if (ahp >= 0.75) continue;  // not worth the mana
                if (ahp < bestAllyHp) { bestAllyHp = ahp; bestWoundedAlly = at; }
            }
            if (bestWoundedAlly) {
                act.useAbilityId = Arena.AB_HEAL;
                act.abilityTargetId = bestWoundedAlly.unit.id;
                mem.intent = "HEAL_ALLY";
            }
        }

        // ── SEEK COVER when under fire and hurt ─────────────────────────
        // If we've been hit recently and we're in the "worrying but not
        // dying" band, try to put an obstacle between us and the shooter
        // instead of trading shots in the open. This is the key to visibly
        // tactical behavior — agents actually use the geometry.
        var underFire = (simT - mem.lastHitT) < 2.0 && mem.threat > 8;
        var hpFrac = u.hp / u.maxHp;
        if (underFire && hpFrac < 0.7) {
            // Collect known threat positions (primary + any other enemy
            // that's clearly shooting us in the last second, but we only
            // record one source at a time — good enough for now).
            var threat = null;
            for (var ti = 0; ti < enemies.length; ti++) {
                if (enemies[ti].unit.id === mem.threatSourceId &&
                    enemies[ti].unit.alive) {
                    threat = enemies[ti]; break;
                }
            }
            if (threat) {
                // Only re-plan cover every ~0.4s to avoid path thrash.
                var needNew = mem.coverX === null ||
                    (simT - (mem.coverPickedT || -99)) > 0.4 ||
                    Math.hypot(agent.x - mem.coverX, agent.z - mem.coverZ) < 0.6;
                if (needNew) {
                    // Team centroid pulls cover choice toward allies so we
                    // don't retreat in isolation.
                    var tcx = 0, tcz = 0, nMates = 0;
                    for (var mi = 0; mi < teammates.length; mi++) {
                        var mt = teammates[mi];
                        if (!mt.unit.alive || mt === agent) continue;
                        tcx += mt.x; tcz += mt.z; nMates++;
                    }
                    if (nMates > 0) { tcx /= nMates; tcz /= nMates; }
                    var cover = AI.findCover(agent, [threat], obstacles, nav, {
                        claimed: AI.claimedCover,
                    });
                    if (cover) {
                        mem.coverX = cover.x;
                        mem.coverZ = cover.z;
                        mem.coverPickedT = simT;
                        AI.claimedCover.push(cover);
                    }
                }
                if (mem.coverX !== null) {
                    agent.setTarget(mem.coverX, mem.coverZ);
                    mem.intent = "COVER";
                    // Self-heal while relocating / hunkered down.
                    if (u.mana >= 25 && hpFrac < 0.65) {
                        act.useAbilityId = Arena.AB_HEAL;
                        act.abilityTargetId = u.id;
                    }
                    // Fire from cover if any enemy is in range + LOS.
                    // Prefer the current focus, fall back to any visible.
                    var shootAt = null;
                    if (bro.ai.game.hasLineOfSight(agent.x, agent.z, focus.x, focus.z, obstacles) &&
                        Math.hypot(focus.x - agent.x, focus.z - agent.z) <= range) {
                        shootAt = focus;
                    } else {
                        for (var ei = 0; ei < enemies.length; ei++) {
                            var en = enemies[ei];
                            if (!en.unit.alive) continue;
                            var ed = Math.hypot(en.x - agent.x, en.z - agent.z);
                            if (ed > range) continue;
                            if (bro.ai.game.hasLineOfSight(agent.x, agent.z, en.x, en.z, obstacles)) {
                                shootAt = en; break;
                            }
                        }
                    }
                    if (shootAt) {
                        act.fireAt = shootAt;
                        // Re-aim toward the shot target so render + FOV match.
                        var sdx = shootAt.x - agent.x, sdz = shootAt.z - agent.z;
                        var smag = Math.max(0.01, Math.hypot(sdx, sdz));
                        mem.aimX = sdx / smag;
                        mem.aimZ = sdz / smag;
                    }
                    return act;
                }
            }
        }
        // Hysteresis: once HP is healthy and we haven't been shot for a
        // while, forget the cover point so we re-engage instead of camping.
        if (hpFrac > 0.85 && (simT - mem.lastHitT) > 2.5) {
            mem.coverX = null;
        }

        var dx = focus.x - agent.x, dz = focus.z - agent.z;
        var dist = Math.hypot(dx, dz);
        var range = u.attackRange;
        var hasLOS = bro.ai.game.hasLineOfSight(
            agent.x, agent.z, focus.x, focus.z, obstacles);

        // ── ABILITIES ─────────────────────────────────────────────────
        // Grenade: 2+ enemies tightly clustered near the focus (splash synergy).
        var cluster = 0;
        for (var c = 0; c < enemies.length; c++) {
            var ce = enemies[c];
            if (!ce.unit.alive) continue;
            if (dist2(ce.x, ce.z, focus.x, focus.z) < 3 * 3) cluster++;
        }
        if (cluster >= 2 && u.mana >= 35 && dist > 3 && dist < 12 && hasLOS) {
            act.useAbilityId = Arena.AB_GRENADE;
            act.abilityTargetId = focus.unit.id;
            mem.intent = "GRENADE";
        }

        // Beam: a second enemy is roughly collinear behind the focus (pierce 2).
        if (act.useAbilityId < 0 && u.mana >= 30 && dist > 2 && dist < 16 && hasLOS) {
            var mag1 = Math.max(0.01, Math.hypot(dx, dz));
            for (var b = 0; b < enemies.length; b++) {
                var be = enemies[b];
                if (be === focus || !be.unit.alive) continue;
                var bx = be.x - agent.x, bz = be.z - agent.z;
                var mag2 = Math.hypot(bx, bz);
                if (mag2 < 0.01) continue;
                // cosine similarity of the two directions
                var cos = (dx * bx + dz * bz) / (mag1 * mag2);
                if (cos > 0.97 && mag2 > mag1) {
                    act.useAbilityId = Arena.AB_BEAM;
                    act.abilityTargetId = focus.unit.id;
                    mem.intent = "BEAM";
                    break;
                }
            }
        }

        // Fireball: mid-range poke when we have LOS but aren't in melee.
        if (act.useAbilityId < 0 && u.mana >= 20 && hasLOS &&
            dist > 4 && dist < 13 && Math.random() < 0.04) {
            act.useAbilityId = Arena.AB_FIREBALL;
            act.abilityTargetId = focus.unit.id;
            mem.intent = "FIREBALL";
        }

        // ── ROLE-BASED FIRING BAND ──────────────────────────────────
        // Healthy units push to the mid-range band; wounded units hold at
        // max range as "support". This creates a visible two-wave front
        // instead of every agent crowding the same firing arc.
        //
        // Front : 0.45–0.85 of range — closes to get kills, absorbs fire.
        // Support: 0.80–0.98 of range — stays back, plinks from safety.
        var isSupport = hpFrac < 0.75;
        mem.role = isSupport ? "support" : "front";
        var tooFar, tooNear;
        if (isSupport) {
            tooFar  = range * 0.98;
            tooNear = range * 0.80;
        } else {
            tooFar  = range * 0.85;
            tooNear = range * 0.45;
        }
        if (!hasLOS) {
            mem.intent = "REPOSITION";
            var repX = focus.x, repZ = focus.z;
            if (nav && !nav.isWalkable(repX, repZ)) {
                var w = AI.findWalkableNear(nav, repX, repZ, 2);
                if (w) { repX = w.x; repZ = w.z; }
            }
            var repSp = AI.spaceFromTeammates(agent, teammates, repX, repZ);
            agent.setTarget(repSp.x, repSp.z);
        } else if (dist > tooFar) {
            mem.intent = "ENGAGE";
            var engSp = AI.spaceFromTeammates(agent, teammates, focus.x, focus.z);
            agent.setTarget(engSp.x, engSp.z);
            act.fireAt = focus;
        } else if (dist < tooNear) {
            // Too close — back off along the reverse vector, snap to walkable.
            mem.intent = "KITE";
            var n = Math.max(0.01, dist);
            var bx = agent.x - (dx / n) * 2.0;
            var bz = agent.z - (dz / n) * 2.0;
            var kiteSp = AI.spaceFromTeammates(agent, teammates, bx, bz);
            if (nav && !nav.isWalkable(kiteSp.x, kiteSp.z)) {
                agent.clearTarget();
            } else {
                agent.setTarget(kiteSp.x, kiteSp.z);
            }
            act.fireAt = focus;
        } else {
            // In firing band with LOS — hold position and shoot. Occasionally
            // strafe a step so we're not a stationary target.
            if (simT - mem.lastFlip > 0.8) {
                mem.perpSign = -mem.perpSign;
                mem.lastFlip = simT;
            }
            var norm2 = Math.max(0.01, dist);
            var strafeX = -dz / norm2 * mem.perpSign * 1.2;
            var strafeZ =  dx / norm2 * mem.perpSign * 1.2;
            var sx = agent.x + strafeX;
            var sz = agent.z + strafeZ;
            var fireSp = AI.spaceFromTeammates(agent, teammates, sx, sz);
            if (nav && nav.isWalkable(fireSp.x, fireSp.z)) agent.setTarget(fireSp.x, fireSp.z);
            else agent.clearTarget();
            mem.intent = "FIRE";
            act.fireAt = focus;
        }
        return act;
    };

    // MCTS wrapper — instantiate once per agent to avoid rebuilding the tree.
    AI.createMcts = function () {
        return bro.ai.game.createMcts({
            iterations: 120,
            budgetMs: 12,
            rolloutHorizon: 12,
            simDt: 0.25,
            actionRepeat: 2,
            uctC: 1.3,
        });
    };

    // MoveDir (0..8) → (moveX, moveZ) unit vector.
    // Convention: 0 stop, 1=N(-Z), 2=NE, 3=E(+X), 4=SE, 5=S(+Z), 6=SW, 7=W(-X), 8=NW.
    var DIRS = [
        [0, 0], [0, -1], [0.707, -0.707], [1, 0], [0.707, 0.707],
        [0, 1], [-0.707, 0.707], [-1, 0], [-0.707, -0.707],
    ];

    // mctsStep: runs mcts.search, caches the action for N frames.
    // `cache` is a mutable object held by caller — { action, ttl }.
    AI.mctsStep = function (agent, world, mcts, cache) {
        if (!cache.action || cache.ttl <= 0) {
            cache.action = mcts.search(world, agent);
            cache.ttl = 15; // re-search every 15 frames (~4 Hz @ 60fps)
        }
        cache.ttl--;
        return cache.action;
    };

    // Apply an MCTS CombatAction + attack/ability on top of scripted movement fallback.
    AI.applyMcts = function (agent, world, ca, dt) {
        var u = agent.unit;
        var dir = DIRS[Math.max(0, Math.min(8, ca.moveDir | 0))];
        var speed = u.effectiveMoveSpeed || u.moveSpeed || 5.2;
        agent.clearTarget();
        agent.applyAction({
            moveX: dir[0] * speed,
            moveZ: dir[1] * speed,
            aimYaw: 0, aimPitch: 0,
            attackTargetId: -1, useAbilityId: -1,
        }, dt);

        var act = { useAbilityId: -1, abilityTargetId: -1, attackTargetId: -1 };
        // Pick any enemy as target for abilities/attacks
        var enemy = world.nearestEnemy(agent);
        if (!enemy || !enemy.unit.alive) return act;
        AI.getMem(agent.unit.id).targetId = enemy.unit.id;

        if (ca.attackSlot >= 0) {
            act.attackTargetId = enemy.unit.id;
        }
        if (ca.abilitySlot >= 0) {
            // Map MCTS ability slot (0..3) to our registered IDs.
            var slot = Math.max(0, Math.min(3, ca.abilitySlot | 0));
            act.useAbilityId = slot;
            act.abilityTargetId = enemy.unit.id;
        }
        return act;
    };
})();
