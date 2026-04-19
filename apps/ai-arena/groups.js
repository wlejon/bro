// groups.js — Blue-team controller. The "mcts" mode flag is preserved for
// compatibility with controls/UI + headless_eval, but the actual policy is
// a hand-tuned scripted AI. Basics fire identically to red's ai.js path
// (BotAim + projectile via spawnProjectile) so the fight is weapon-symmetric
// — any win here comes from target selection, ability use, and positioning,
// not from hitscan / no-LOS asymmetries.
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
            // Same BotAim tuning red uses in ai.js → shot convention is
            // identical on both teams: 15 Hz target resample, 5 rad/s
            // turn cap, 8.6° fire cone. A mover with red stats makes
            // both teams whiff by the same amount.
            aim: BotAim.create({ turnSpeed: 5.0, sampleHz: 15, fireConeRad: 0.15 }),
            shootCd: 0, lastTickT: -1,
            coverX: null, coverZ: null, coverPickedT: -99,
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

    // Team focus: lowest-HP enemy visible (LOS) from any blue within
    // 1.8× attackRange. Mirrors ai.js AI.chooseTeamFocus so both sides
    // coordinate off the same signal.
    function chooseTeamFocus(blue, red) {
        var PERCEPT_MUL = 1.8;
        var best = null, bestHp = Infinity;
        for (var i = 0; i < red.length; i++) {
            var e = red[i];
            var seen = false;
            for (var j = 0; j < blue.length; j++) {
                var b = blue[j];
                var pr = (b.unit.attackRange || 9) * PERCEPT_MUL;
                if (dist2(b.x, b.z, e.x, e.z) > pr * pr) continue;
                if (hasLOS(b.x, b.z, e.x, e.z)) { seen = true; break; }
            }
            if (!seen) continue;
            if (e.unit.hp < bestHp) { bestHp = e.unit.hp; best = e; }
        }
        if (best) return best;
        // No LOS fallback: nearest red to any blue.
        var bd = Infinity;
        for (var k = 0; k < red.length; k++) {
            for (var m = 0; m < blue.length; m++) {
                var d = dist2(blue[m].x, blue[m].z, red[k].x, red[k].z);
                if (d < bd) { bd = d; best = red[k]; }
            }
        }
        return best;
    }

    // Per-agent fire target: prefer the team focus when it's in-range + LOS
    // (coordinated burst beats split damage); else lowest-HP enemy in range;
    // else the team focus as a travel destination.
    function pickFireTarget(agent, enemies, teamFocus) {
        var r = agent.unit.attackRange || 9;
        var r2 = r * r;
        if (teamFocus && teamFocus.unit.alive
            && dist2(agent.x, agent.z, teamFocus.x, teamFocus.z) <= r2
            && hasLOS(agent.x, agent.z, teamFocus.x, teamFocus.z)) {
            return teamFocus;
        }
        var inRange = null, inRangeHp = Infinity;
        var nearest = null, nearestD = Infinity;
        for (var i = 0; i < enemies.length; i++) {
            var e = enemies[i];
            var d2 = dist2(agent.x, agent.z, e.x, e.z);
            if (d2 < nearestD) { nearestD = d2; nearest = e; }
            if (d2 <= r2 && e.unit.hp < inRangeHp) { inRangeHp = e.unit.hp; inRange = e; }
        }
        return inRange || teamFocus || nearest;
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
    // Fire a basic-shot projectile using the same stats red uses in ai.js:
    // speed 18, radius 0.22, damage 9, 1.2s life. BotAim gates the shot on
    // reticle-on-target (fireCone check). Gun cooldown is a JS-side mirror
    // of 1/attacksPerSec; Unit::attackCooldown isn't consumed here because
    // resolveAttack isn't the fire path — spawnProjectile is.
    function fireBasic(agent, target, m, world) {
        if (m.shootCd > 0) return;
        if (!target || !target.unit.alive) return;
        var u = agent.unit;
        var dx = target.x - agent.x, dz = target.z - agent.z;
        var d = Math.sqrt(dx * dx + dz * dz);
        if (d > u.attackRange) return;
        if (!hasLOS(agent.x, agent.z, target.x, target.z)) return;
        if (!BotAim.canFireAt(m.aim, agent.x, 0, agent.z, target.x, 0, target.z)) return;

        var f = BotAim.forward(m.aim);
        var PSPEED = 18;
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
    }

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
            if (hpFrac < 0.55) {
                if (world.resolveAbility(agent, AB_HEAL, u.id)) return;
            }
            var hurt = null, hurtHp = 0.55;
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

        // FIREBALL — filler only when we're not going to need mana for
        // a heal soon. ai.js spams fireball at 6 %/frame which drains
        // its own mana pool; blue's edge in longer trades is refusing
        // to spend mana on filler when heal might be needed.
        if (hpFrac > 0.75 && u.mana >= 50
            && tdist > 3 && tdist < 14
            && hasLOS(agent.x, agent.z, target.x, target.z)
            && !friendlyOnLine(agent, target, teammates, 0.6)) {
            world.resolveAbility(agent, AB_FIREBALL, target.unit.id);
        }
    }

    // Spacing beats AoE: red's grenade splashRadius is 2.5 and beam pierces
    // collinear targets, so anything under ~3 makes us share damage. Push
    // the destination off any teammate inside SPACING.
    var SPACING = 3.2;
    function spaceOut(agent, x, z, teammates) {
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
    }

    // Positioning. Three modes, priority-ordered:
    //   1. Panic flee when HP < 25% AND we can't heal soon — run and hide.
    //   2. Seek cover when threat is active (damage taken in last 2s) and
    //      HP < 70% — break LOS to the shooter.
    //   3. Hold at max attackRange on `target`, strafing to dodge.
    //
    // Basics + abilities fire off LOS; closing inside range buys no extra
    // damage (1.4/s cap), so we sit at the outer edge to minimise red's
    // projectile hit rate (longer travel → larger lead error).
    // Returns { x, z, mode } where mode is one of "retreat" / "cover" /
    // "advance" / "strafe" / "idle". The caller uses mode to decide
    // whether it's safe to layer dodge motion on top.
    function computeMoveTarget(agent, target, enemies, teammates, nav, simT, m) {

        var u = agent.unit;
        var hpFrac = u.hp / u.maxHp;
        var r = u.attackRange || 9;

        var memShared = AI.getMem(u.id);  // shared with ai.js threat tracker
        var threatSrc = null;
        // Lower threat threshold (was >6): any recent damage source is
        // worth breaking LOS from. Even a single incoming basic means
        // red has a firing line; taking cover resets their aim cycle.
        if (memShared.threatSourceId >= 0
            && (simT - memShared.lastHitT) < 2.5
            && memShared.threat > 1) {
            for (var ti = 0; ti < enemies.length; ti++) {
                if (enemies[ti].unit.id === memShared.threatSourceId) {
                    threatSrc = enemies[ti]; break;
                }
            }
        }

        // 1) Flee-rally when wounded. Bumped from 0.35 → 0.45: trade HP
        // for position earlier so we can heal behind cover instead of
        // outtrading from 30%. Too eager a threshold (0.55+) starts
        // bleeding DPS uptime; 0.45 strikes the balance.
        if (hpFrac < 0.45 && enemies.length > 0) {
            var nearest = null, nearestD = Infinity;
            for (var e = 0; e < enemies.length; e++) {
                var ed = dist2(agent.x, agent.z, enemies[e].x, enemies[e].z);
                if (ed < nearestD) { nearestD = ed; nearest = enemies[e]; }
            }
            var rally = null, rallyScore = -Infinity;
            for (var mi = 0; mi < teammates.length; mi++) {
                var mt = teammates[mi];
                if (mt === agent) continue;
                var mHp = mt.unit.hp / mt.unit.maxHp;
                if (mHp < 0.55) continue;
                var score = mHp * 100 - Math.hypot(mt.x - agent.x, mt.z - agent.z);
                if (score > rallyScore) { rallyScore = score; rally = mt; }
            }
            if (rally && nearest) {
                var avx = rally.x - nearest.x, avz = rally.z - nearest.z;
                var avm = Math.max(0.01, Math.hypot(avx, avz));
                var anchorX = rally.x + (avx / avm) * 1.8;
                var anchorZ = rally.z + (avz / avm) * 1.8;
                var rc = AI.findCover(agent, [nearest], Arena.OBSTACLES, nav,
                    { anchorX: anchorX, anchorZ: anchorZ, claimed: AI.claimedCover });
                if (rc) {
                    AI.claimedCover.push(rc);
                    var rcSp = spaceOut(agent, rc.x, rc.z, teammates);
                    rcSp.mode = "retreat"; return rcSp;
                }
                var rcA = spaceOut(agent, anchorX, anchorZ, teammates);
                rcA.mode = "retreat"; return rcA;
            }
            if (nearest) {
                var fx = agent.x + (agent.x - nearest.x) * 0.5;
                var fz = agent.z + (agent.z - nearest.z) * 0.5;
                var fSp = spaceOut(agent, clamp(fx, -19, 19), clamp(fz, -19, 19), teammates);
                fSp.mode = "retreat"; return fSp;
            }
        }

        // Seek cover AT ANY HP when actively being shot at. The old 0.85
        // threshold let full-HP blues stand and trade; now threat triggers
        // cover regardless of HP. Blue out-trades red in the long run
        // because mana discipline + heal economy, not toe-to-toe DPS.
        if (threatSrc) {
            var needFresh = m.coverX === null
                || (simT - m.coverPickedT) > 0.5
                || Math.hypot(agent.x - m.coverX, agent.z - m.coverZ) < 0.7;
            if (needFresh) {
                var cover = AI.findCover(agent, [threatSrc], Arena.OBSTACLES, nav,
                    { claimed: AI.claimedCover });
                if (cover) {
                    m.coverX = cover.x; m.coverZ = cover.z;
                    m.coverPickedT = simT;
                    AI.claimedCover.push(cover);
                }
            }
            if (m.coverX !== null) {
                var cSp = spaceOut(agent, m.coverX, m.coverZ, teammates);
                cSp.mode = "cover"; return cSp;
            }
            var tvx = agent.x - threatSrc.x, tvz = agent.z - threatSrc.z;
            var tvm = Math.hypot(tvx, tvz) || 1;
            var rx = clamp(agent.x + (tvx / tvm) * 3.5, -19, 19);
            var rz = clamp(agent.z + (tvz / tvm) * 3.5, -19, 19);
            var rSp = spaceOut(agent, rx, rz, teammates);
            rSp.mode = "cover"; return rSp;
        }
        if (!threatSrc && hpFrac > 0.85) { m.coverX = null; }

        if (!target) return null;
        var dx2 = target.x - agent.x, dz2 = target.z - agent.z;
        var d2 = Math.hypot(dx2, dz2);
        var desiredD = r * 0.95;
        if (d2 > r * 1.02) {
            var lead = d2 - desiredD;
            if (lead < 0.3) return null;
            var tx = clamp(agent.x + (dx2 / d2) * lead, -19, 19);
            var tz = clamp(agent.z + (dz2 / d2) * lead, -19, 19);
            var aSp = spaceOut(agent, tx, tz, teammates);
            aSp.mode = "advance"; return aSp;
        }
        if (simT - m.lastFlip > 0.8) {
            m.strafeSign = -m.strafeSign;
            m.lastFlip = simT;
        }
        var norm = Math.max(0.01, d2);
        var sx = agent.x + (-dz2 / norm) * m.strafeSign * 1.4;
        var sz = agent.z + ( dx2 / norm) * m.strafeSign * 1.4;
        var strSp = spaceOut(agent, sx, sz, teammates);
        strSp.mode = "strafe"; return strSp;
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

        var teamFocus = chooseTeamFocus(blue, red);

        for (var j = 0; j < blue.length; j++) {
            var agent = blue[j];
            var m = getMem(agent.unit.id);
            var prevT = m.lastTickT < 0 ? simT : m.lastTickT;
            var localDt = Math.max(0.001, Math.min(0.2, simT - prevT));
            m.lastTickT = simT;
            if (m.shootCd > 0) m.shootCd -= localDt;

            var target = pickFireTarget(agent, red, teamFocus);

            // Aim → project like red's tryAimedShot. Same BotAim cone/
            // turn-lag/LOS/travel-time convention both sides share.
            if (target) {
                var aDx = target.x - agent.x, aDz = target.z - agent.z;
                var aYaw = Math.atan2(aDx, -aDz);
                BotAim.requestAim(m.aim, simT, aYaw, 0);
            }
            BotAim.tick(m.aim, localDt);
            if (target) fireBasic(agent, target, m, world);

            // ABILITIES.
            castAbilities(agent, target, red, blue, world);

            // MOVEMENT.
            var mt = computeMoveTarget(agent, target, red, blue, state.nav, simT, m);


            // Zig-zag on top of retreat / cover moves. Blue isn't firing
            // during those anyway (either out of LOS behind cover, or
            // running the other way), so sidestepping is free HP. During
            // advance / strafe-in-range the agent is holding aim on the
            // target; a dodge there breaks the aim cone and costs more
            // DPS than it saves. User's insight: "in retreat, zig-zagging
            // would save them" — the gate enforces exactly that.
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

    // Exposed so the FOV-cone renderer (scene_setup.js) can read blue's
    // aim orientation. ai.js units store their aim in AI.memory; blue
    // units store theirs here since their bindings are detached and
    // AI.think never runs for them.
    Groups.aimFor = function (agentId) {
        var m = mem[agentId];
        return (m && m.aim) ? m.aim : null;
    };

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
