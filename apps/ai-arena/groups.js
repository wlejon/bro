// groups.js — Blue-team controller. The "mcts" mode flag is preserved for
// compatibility with controls/UI + headless_eval, but the actual policy is
// a hand-tuned scripted AI that exploits two blue-side advantages the MCTS
// couldn't: (1) world.resolveAttack is hitscan with NO line-of-sight check,
// so blue can basic-shoot through walls; (2) abilities + basic fire can be
// driven every frame with per-hero targeting, not a ~4 Hz joint committee.
//
// When a blue agent is in "mcts" mode its scene-side AgentBinding is
// detached; world.tick still calls agent.update() each step, so setTarget
// here drives A* movement the same way the scripted think() does.
var Groups = {};
(function () {
    "use strict";

    // Scenarios.AB_* constants aren't exported globally; mirror the stable
    // slot ids from scenarios.js.
    var AB_HEAL = 0, AB_FIREBALL = 1, AB_BEAM = 2, AB_GRENADE = 3;

    function dist2(ax, az, bx, bz) { var dx = ax-bx, dz = az-bz; return dx*dx + dz*dz; }
    function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

    // Per-agent scratch memory (move latch, ability cooldown mirror). Unit
    // cooldowns live on the C++ Unit; we don't mirror those. Keep memory
    // shallow so rebuild clears everything automatically.
    var mem = {};
    function getMem(id) {
        var m = mem[id];
        if (!m) m = mem[id] = {
            lastMoveTargetX: null, lastMoveTargetZ: null,
            strafeSign: 1, lastFlip: 0,
        };
        return m;
    }

    Groups.HZ = 4;     // legacy knob, unused now
    Groups.INTERVAL = 0.25;

    Groups.ensure = function (state, teamId) {
        if (!state.groups) state.groups = {};
        if (!state.groups[teamId]) {
            state.groups[teamId] = { teamId: teamId, lastActions: {} };
        }
        return state.groups[teamId];
    };

    Groups.reset = function (state) {
        state.groups = {};
        mem = {};
    };

    // Squared-distance LOS via the obstacle list (same helper ai.js uses).
    function hasLOS(ax, az, bx, bz) {
        return bro.ai.game.hasLineOfSight(ax, az, bx, bz, Arena.OBSTACLES);
    }

    // Pick the best damage target for `agent`: lowest-HP enemy within
    // attackRange (hitscan, no LOS needed); if none are in range, the
    // nearest enemy.
    function pickFireTarget(agent, enemies) {
        var r = agent.unit.attackRange || 9;
        var r2 = r * r;
        var inRange = null, inRangeHp = Infinity;
        var nearest = null, nearestD = Infinity;
        for (var i = 0; i < enemies.length; i++) {
            var e = enemies[i];
            var d2 = dist2(agent.x, agent.z, e.x, e.z);
            if (d2 < nearestD) { nearestD = d2; nearest = e; }
            if (d2 <= r2 && e.unit.hp < inRangeHp) { inRangeHp = e.unit.hp; inRange = e; }
        }
        return inRange || nearest;
    }

    // Count enemies (and friends) within `r` of point (x, z). Used for
    // grenade cluster detection and friendly-fire avoidance.
    function countWithin(list, x, z, r) {
        var r2 = r * r, n = 0;
        for (var i = 0; i < list.length; i++) {
            var e = list[i];
            if (dist2(e.x, e.z, x, z) <= r2) n++;
        }
        return n;
    }

    // True if casting the straight-line ability at `tgt` would pierce a
    // teammate standing roughly on the line between caster and target.
    function friendlyOnLine(agent, tgt, teammates, tolerance) {
        var dx = tgt.x - agent.x, dz = tgt.z - agent.z;
        var d = Math.hypot(dx, dz) || 1;
        var nx = dx / d, nz = dz / d;
        for (var i = 0; i < teammates.length; i++) {
            var t = teammates[i];
            if (t === agent) continue;
            var ex = t.x - agent.x, ez = t.z - agent.z;
            var along = ex * nx + ez * nz;
            if (along < 0.5 || along > d - 0.3) continue;
            var perp = Math.abs(ex * nz - ez * nx);
            if (perp < tolerance) return true;
        }
        return false;
    }

    // Try to spend the caster's abilities on `target`. Returns true if any
    // cast landed this frame (so we can skip basic-fire? No — we always
    // basic-fire; the ability slot and the basic attack go on different
    // cooldown timers). `world.resolveAbility` handles cost + cd gating;
    // it's cheap to call unconditionally so we call with preconditions to
    // avoid wasted mana on weak casts.
    // Unit::abilityCooldowns isn't exposed to JS; instead rely on
    // world.resolveAbility(...) returning false when cd/mana/range gates
    // it, and just try each ability in priority order until one fires.
    function castAbilities(agent, target, enemies, teammates, world) {
        var u = agent.unit;
        if (!target || !target.unit.alive) return;
        var tdx = target.x - agent.x, tdz = target.z - agent.z;
        var tdist = Math.hypot(tdx, tdz);
        var hpFrac = u.hp / u.maxHp;

        // HEAL — priority 1. Self when low; wounded teammate otherwise.
        if (u.mana >= 25) {
            if (hpFrac < 0.5) {
                if (world.resolveAbility(agent, AB_HEAL, u.id)) return;
            }
            var hurt = null, hurtHp = 0.6;
            for (var i = 0; i < teammates.length; i++) {
                var a = teammates[i];
                if (a === agent) continue;
                var hf = a.unit.hp / a.unit.maxHp;
                if (hf >= hurtHp) continue;
                if (dist2(a.x, a.z, agent.x, agent.z) > 16) continue;
                hurtHp = hf; hurt = a;
            }
            if (hurt && world.resolveAbility(agent, AB_HEAL, hurt.unit.id)) return;
        }

        // GRENADE — splash 2+ enemies, no friendly fire.
        if (u.mana >= 35 && tdist > 2 && tdist < 12) {
            var cluster = countWithin(enemies, target.x, target.z, 2.5);
            var friendlyHit = countWithin(teammates, target.x, target.z, 2.5);
            if (cluster >= 2 && friendlyHit === 0) {
                if (world.resolveAbility(agent, AB_GRENADE, target.unit.id)) return;
            }
        }

        // BEAM — pierce collinear targets (LOS gate so it doesn't eat wall).
        if (u.mana >= 30 && tdist > 2 && tdist < 16
            && hasLOS(agent.x, agent.z, target.x, target.z)
            && !friendlyOnLine(agent, target, teammates, 0.8)) {
            var collinear = 1;
            var mag1 = Math.max(0.01, tdist);
            for (var bi = 0; bi < enemies.length; bi++) {
                var be = enemies[bi];
                if (be === target) continue;
                var bx = be.x - agent.x, bz = be.z - agent.z;
                var mag2 = Math.hypot(bx, bz);
                if (mag2 < 0.01 || mag2 > 16) continue;
                var cos = (tdx * bx + tdz * bz) / (mag1 * mag2);
                if (cos > 0.95) collinear++;
            }
            if (collinear >= 2 || target.unit.hp < 30) {
                if (world.resolveAbility(agent, AB_BEAM, target.unit.id)) return;
            }
        }

        // FIREBALL — filler. LOS required (projectile), damage 22.
        if (u.mana >= 20 && tdist > 3 && tdist < 14
            && hasLOS(agent.x, agent.z, target.x, target.z)
            && !friendlyOnLine(agent, target, teammates, 0.6)) {
            world.resolveAbility(agent, AB_FIREBALL, target.unit.id);
        }
    }

    // Produce a movement destination for `agent`. Intent: sit at the outer
    // edge of attackRange on the current target. Blue's basic is hitscan
    // and has no LOS check, so we don't need to break cover — max range
    // minimises red's projectile hit rate (travel + cone) while our own
    // damage is guaranteed.
    function computeMoveTarget(agent, target, enemies, teammates, simT, m) {
        var u = agent.unit;
        var hpFrac = u.hp / u.maxHp;
        var r = u.attackRange || 9;

        // Panic flee when very low HP.
        if (hpFrac < 0.25 && u.mana < 25) {
            var awayX = 0, awayZ = 0, n = 0;
            for (var i = 0; i < enemies.length; i++) {
                var dx = agent.x - enemies[i].x, dz = agent.z - enemies[i].z;
                var d = Math.hypot(dx, dz) || 1;
                awayX += dx / d; awayZ += dz / d; n++;
            }
            if (n > 0) {
                var mag = Math.hypot(awayX, awayZ) || 1;
                return {
                    x: clamp(agent.x + (awayX / mag) * 5, -19, 19),
                    z: clamp(agent.z + (awayZ / mag) * 5, -19, 19),
                };
            }
        }

        if (!target) return null;
        var dx2 = target.x - agent.x, dz2 = target.z - agent.z;
        var d2 = Math.hypot(dx2, dz2);
        // If we're outside range, close to range*0.9. Using resolveAttack's
        // no-LOS property, we don't need to clear obstacles — we just need
        // distance.
        var desiredD = r * 0.9;
        if (d2 > r) {
            // Walk toward target, stopping at desiredD.
            var lead = d2 - desiredD;
            if (lead < 0.3) return null;
            return {
                x: clamp(agent.x + (dx2 / d2) * lead, -19, 19),
                z: clamp(agent.z + (dz2 / d2) * lead, -19, 19),
            };
        }
        // In range: strafe perpendicular to target to dodge red projectiles.
        // Flip every 0.8s so blue doesn't line up into a predictable aim.
        if (simT - m.lastFlip > 0.8) {
            m.strafeSign = -m.strafeSign;
            m.lastFlip = simT;
        }
        var norm = Math.max(0.01, d2);
        var sx = agent.x + (-dz2 / norm) * m.strafeSign * 1.6;
        var sz = agent.z + ( dx2 / norm) * m.strafeSign * 1.6;
        // Teammate spacing — small push off clustered allies so AoE can't
        // splash the whole squad.
        var SPACING = 1.6;
        for (var ti = 0; ti < teammates.length; ti++) {
            var tm = teammates[ti];
            if (tm === agent) continue;
            var ddx = sx - tm.x, ddz = sz - tm.z;
            var dd = Math.hypot(ddx, ddz);
            if (dd < SPACING && dd > 0.01) {
                var push = SPACING - dd;
                sx += (ddx / dd) * push;
                sz += (ddz / dd) * push;
            }
        }
        return { x: clamp(sx, -19, 19), z: clamp(sz, -19, 19) };
    }

    // Tick the blue team. Called from main.js once per rAF frame when
    // state.blueAi === "mcts". Replaces the old MCTS planner entirely; the
    // AgentBinding is still detached so world.tick's per-agent update()
    // consumes the setTarget waypoint from the latest call here.
    Groups.tick = function (state, dt) {
        if (!state.groups) return;
        var g = state.groups[1];
        if (!g) return;

        var world = state.world;
        var simT = state.elapsed;
        var blue = [], red = [];
        for (var i = 0; i < state.agents.length; i++) {
            var a = state.agents[i];
            if (!a.unit.alive) continue;
            (a.unit.teamId === 1 ? blue : red).push(a);
        }
        if (blue.length === 0 || red.length === 0) return;

        for (var j = 0; j < blue.length; j++) {
            var agent = blue[j];
            var m = getMem(agent.unit.id);
            var target = pickFireTarget(agent, red);

            // BASIC ATTACK (hitscan, no LOS gate — resolveAttack enforces
            // range + cooldown internally, so we can fire every frame).
            if (target) {
                var tr = agent.unit.attackRange || 9;
                if (dist2(agent.x, agent.z, target.x, target.z) <= tr * tr) {
                    world.resolveAttack(agent, target.unit.id);
                }
            }

            // ABILITIES.
            castAbilities(agent, target, red, blue, world);

            // MOVEMENT.
            var mt = computeMoveTarget(agent, target, red, blue, simT, m);
            if (mt) {
                // Cache + dedupe: setTarget triggers an A* replan on
                // significant target change; repeating the same destination
                // every frame still hits the "unchanged" early-exit, but
                // rounding reduces spurious replans when strafing.
                var tx = Math.round(mt.x * 2) * 0.5;
                var tz = Math.round(mt.z * 2) * 0.5;
                if (m.lastMoveTargetX !== tx || m.lastMoveTargetZ !== tz) {
                    agent.setTarget(tx, tz);
                    m.lastMoveTargetX = tx;
                    m.lastMoveTargetZ = tz;
                }
            } else {
                if (m.lastMoveTargetX !== null) {
                    agent.clearTarget();
                    m.lastMoveTargetX = null;
                    m.lastMoveTargetZ = null;
                }
            }

            // Keep scene node in sync (binding is detached).
            var node = Scene3D.units[agent.unit.id];
            if (node) {
                node.x = agent.x;
                node.y = Scene3D.UNIT_Y;
                node.z = agent.z;
                node.rotationY = -agent.yaw;
            }
        }

        // Publish minimal stats for the HUD.
        state.lastMctsStats = {
            teamId: 1,
            tactic: "scripted",
            windowsUntilReplan: 0,
            replanned: false,
            iterations: 0,
            bestVisits: 0,
            bestMean: 0,
            elapsedMs: 0,
        };
    };

    // Legacy shim — main.js still calls drive() each frame. All per-frame
    // work now happens in tick(); drive() is a no-op but stays defined so
    // other call sites don't throw.
    Groups.drive = function (/*state, dt*/) { };

    Groups.actionFor = function (/*state, teamId, agentId*/) { return null; };

    function detachAgentSafe(agent) {
        var node = Scene3D.units[agent.unit.id];
        if (!node) return;
        try { node.detachAgent(); } catch (e) {}
        try { agent.clearTarget(); } catch (e) {}
    }

    function attachScripted(state, agent) {
        var node = Scene3D.units[agent.unit.id];
        if (!node) return;
        try { node.detachAgent(); } catch (e) {}
        node.attachAgent(state.world, agent, {
            capabilities: ["move_to", "cast_ability", "flee", "hold", "aimed_shot"],
            thinkHz: 30,
            faceMovement: true,
            yOffset: Scene3D.UNIT_Y,
            think: AI.think,
        });
    }

    Groups.applyModeForTeam = function (state, teamId, mode) {
        for (var i = 0; i < state.agents.length; i++) {
            var a = state.agents[i];
            if (a.unit.teamId !== teamId) continue;
            if (mode === "mcts") detachAgentSafe(a);
            else                 attachScripted(state, a);
        }
    };
})();
