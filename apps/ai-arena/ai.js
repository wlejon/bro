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
        };
        return m;
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
    AI.chooseTeamFocus = function (team, enemies, obstacles) {
        var best = null, bestHp = Infinity;
        for (var i = 0; i < enemies.length; i++) {
            var e = enemies[i];
            if (!e.unit.alive) continue;
            var seen = false;
            for (var j = 0; j < team.length; j++) {
                var t = team[j];
                if (!t.unit.alive) continue;
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
    AI.scriptedTactical = function (agent, world, nav, enemies, focus, obstacles, simT) {
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

        // ── FLEE + HEAL when wounded ──────────────────────────────────
        if (u.hp / u.maxHp < 0.35) {
            mem.intent = "FLEE";
            if (u.mana >= 25) {
                act.useAbilityId = Arena.AB_HEAL;
                act.abilityTargetId = u.id;
                mem.intent = "HEAL";
            }
            if (closest) {
                var fx = clamp(agent.x + (agent.x - closest.x) * 3, -19, 19);
                var fz = clamp(agent.z + (agent.z - closest.z) * 3, -19, 19);
                if (nav && !nav.isWalkable(fx, fz)) {
                    var fw = AI.findWalkableNear(nav, fx, fz, 3);
                    if (fw) { fx = fw.x; fz = fw.z; }
                }
                agent.setTarget(fx, fz);
            } else {
                agent.clearTarget();
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

        // ── MOVEMENT ─────────────────────────────────────────────────
        // Ideal band: stand at 75-95% of shoot range so we can fire but aren't
        // hugging the enemy. Close in or reposition when outside the band, kite
        // when inside it.
        var tooFar  = range * 0.95;
        var tooNear = range * 0.45;
        if (!hasLOS) {
            mem.intent = "REPOSITION";
            if (nav && !nav.isWalkable(focus.x, focus.z)) {
                var w = AI.findWalkableNear(nav, focus.x, focus.z, 2);
                if (w) { agent.setTarget(w.x, w.z); return act; }
            }
            agent.setTarget(focus.x, focus.z);
        } else if (dist > tooFar) {
            mem.intent = "ENGAGE";
            agent.setTarget(focus.x, focus.z);
            act.fireAt = focus;
        } else if (dist < tooNear) {
            // Too close — back off along the reverse vector, snap to walkable.
            mem.intent = "KITE";
            var n = Math.max(0.01, dist);
            var bx = clamp(agent.x - (dx / n) * 2.0, -19, 19);
            var bz = clamp(agent.z - (dz / n) * 2.0, -19, 19);
            if (nav && !nav.isWalkable(bx, bz)) {
                agent.clearTarget();
            } else {
                agent.setTarget(bx, bz);
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
            var sx = clamp(agent.x + strafeX, -19, 19);
            var sz = clamp(agent.z + strafeZ, -19, 19);
            if (nav && nav.isWalkable(sx, sz)) agent.setTarget(sx, sz);
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
