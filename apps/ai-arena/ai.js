// ai.js — Scripted policy as a think(self, world) callback for the
// capability-based AgentBinding. Each unit runs this at thinkHz from the
// scene's AI tick; it picks exactly one capability action per tick
// (moveTo / cast / flee / hold) and — as a side effect — fires a basic shot
// directly via world.spawnProjectile when BotAim is on target and the shoot
// cooldown is ready. The aimed_shot behavior is declared as a registered
// custom capability for documentation and future use (JS-registered caps
// aren't directly invocable from `self` today).
//
// Shared per-frame state (team focus, teammate/enemy rosters, claimedCover)
// is refreshed in AI.updateShared(state) from loop.js before bindings tick,
// so think() reads ready snapshots rather than recomputing per agent.
var AI = {};
(function () {
    "use strict";

    // ───────────────────────────────────────────────────────────────────────
    // Scalar helpers
    // ───────────────────────────────────────────────────────────────────────
    function dist2(ax, az, bx, bz) { var dx = ax-bx, dz = az-bz; return dx*dx + dz*dz; }
    function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

    // ───────────────────────────────────────────────────────────────────────
    // Per-agent memory (threat, aim, flee latch, cover latch, intent label)
    // ───────────────────────────────────────────────────────────────────────
    AI.memory = {};
    AI.getMem = function (id) {
        var m = AI.memory[id];
        if (!m) m = AI.memory[id] = {
            intent: "idle", targetId: null,
            shootCd: 0, lastThinkT: -1,
            // Gun-lag aim: 15Hz target resample + turn-rate-limited rotation.
            aim: BotAim.create({ turnSpeed: 5.0, sampleHz: 15, fireConeRad: 0.15 }),
            threat: 0, threatSourceId: -1, lastHitT: -99,
            role: "front", coverX: null, coverZ: null, coverPickedT: -99,
            fleeX: null, fleeZ: null, fleePickedT: -99,
            perpSign: 1, lastFlip: -99,
            // Mirror of unit.abilityCooldowns (not exposed to JS yet). We seed
            // it on successful cast; decay per think tick.
            abCd: [0, 0, 0, 0, 0, 0, 0, 0],
        };
        return m;
    };

    AI.decayThreat = function (mem, dt) {
        var decay = Math.exp(-dt / 1.5);  // half-life ~1s
        mem.threat *= decay;
        if (mem.threat < 0.5) mem.threatSourceId = -1;
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

    // Per-tick fan-out: wounded ralliers append their chosen cover so
    // downstream agents avoid piling on. Reset at the top of each frame.
    AI.claimedCover = [];
    AI.resetClaimedCover = function () { AI.claimedCover.length = 0; };

    // ───────────────────────────────────────────────────────────────────────
    // Perception helpers (LOS + target selection)
    // ───────────────────────────────────────────────────────────────────────
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
        // No LOS fallback: nearest enemy to any teammate.
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

    // ───────────────────────────────────────────────────────────────────────
    // Cover / movement helpers
    // ───────────────────────────────────────────────────────────────────────
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

    // Find a walkable cell that breaks LOS from every listed threat. Accepts
    // opts.anchorX/anchorZ (ring center), opts.claimed (points to avoid),
    // opts.minThreatDistance. Returns {x,z} or null.
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

    AI.pushOutOfEnemyRange = function (x, z, enemies, slack) {
        slack = slack || 1.15;
        for (var iter = 0; iter < 4; iter++) {
            var worst = null, worstExcess = 0;
            for (var ei = 0; ei < enemies.length; ei++) {
                var e = enemies[ei];
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
            x = clamp(x, -19, 19); z = clamp(z, -19, 19);
        }
        return { x: x, z: z };
    };

    // ───────────────────────────────────────────────────────────────────────
    // Per-frame shared state + capability registration
    // ───────────────────────────────────────────────────────────────────────
    AI.shared = {
        world: null, nav: null, obstacles: null,
        teams: [[], []], teamFocus: [null, null], simT: 0, byId: null,
    };

    // Per-team "mood" tunings consumed by think(). Portfolio search writes
    // these before a rollout / committed think to steer scripted behavior
    // toward a candidate tactic. Missing team → DEFAULT_TUNING used.
    AI.DEFAULT_TUNING = {
        fleeHpFrac: 0.35,
        engageDistMul: 0.85,
        kiteDistMul: 0.45,
        supportEngageDistMul: 0.98,
        supportKiteDistMul: 0.80,
        manaReserveHeal: 25,      // min mana before trying HEAL
        fireballMinHp: 0.85,      // own-HP threshold to spend mana on fillers
    };
    AI.tuningByTeam = [null, null];
    AI.tuningFor = function (teamId) {
        return AI.tuningByTeam[teamId] || AI.DEFAULT_TUNING;
    };

    // Declarative custom capability for documentation + future-proofing. The
    // JS binding layer doesn't yet let self.* methods invoke registered caps,
    // so the actual gun-lag shot is implemented inline in think() below.
    AI.CAP_AIMED_SHOT = -1;
    AI.registerCapabilities = function () {
        if (AI.CAP_AIMED_SHOT < 0) {
            AI.CAP_AIMED_SHOT = bro.ai.game.registerCapability("aimed_shot", {
                gate: function () { return true; },
                start: function () { },
                advance: function () { return true; },
            });
        }
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

    // ───────────────────────────────────────────────────────────────────────
    // Aim + direct-fire (side effect from think())
    // ───────────────────────────────────────────────────────────────────────
    function requestAimTowards(mem, simT, dx, dz) {
        var yaw = Math.atan2(dx, -dz); // FPS yaw convention (0 = -Z).
        BotAim.requestAim(mem.aim, simT, yaw, 0);
    }

    // Cast + seed the JS-side cooldown mirror. Call only after checking
    // preconditions (cooldown, mana, range, LOS) so the mirror doesn't drift.
    function doCast(self, mem, slot, tid) {
        self.cast(slot, tid);
        var abs = Arena.scenario.abilities;
        mem.abCd[slot] = (abs[slot] && abs[slot].cooldown) || 1;
    }

    // Spawn a basic-shot projectile along BotAim.forward if ready. Returns
    // true if we fired. Kept separate from the per-tick action selection —
    // firing is a *side effect* of think, running in parallel with movement.
    function tryAimedShot(agent, mem, target) {
        if (mem.shootCd > 0) return false;
        if (!target || !target.unit.alive) return false;
        var u = agent.unit;
        var dx = target.x - agent.x, dz = target.z - agent.z;
        var d = Math.sqrt(dx*dx + dz*dz);
        if (d > u.attackRange) return false;
        if (!bro.ai.game.hasLineOfSight(agent.x, agent.z, target.x, target.z, AI.shared.obstacles)) return false;
        if (!BotAim.canFireAt(mem.aim, agent.x, 0, agent.z, target.x, 0, target.z)) return false;

        var f = BotAim.forward(mem.aim);
        var PSPEED = 18;
        AI.shared.world.spawnProjectile({
            ownerId: u.id, teamId: u.teamId,
            x: agent.x + f.x * (u.radius + 0.4),
            z: agent.z + f.z * (u.radius + 0.4),
            vx: f.x * PSPEED, vz: f.z * PSPEED,
            speed: PSPEED, radius: 0.22,
            damage: 9, remainingLife: 1.2,
            kind: "physical", mode: "single",
        });
        mem.shootCd = 1.0 / Math.max(0.1, u.attacksPerSec);
        return true;
    }

    // ───────────────────────────────────────────────────────────────────────
    // think(self, world) — called by the AgentBinding at thinkHz. This is
    // the scripted algorithm; other algorithms live in their own think
    // functions (see apps/ai-arena/agents/).
    // ───────────────────────────────────────────────────────────────────────
    AI.think = function (self, world) {
        var agent = self.agent;
        var u = agent.unit;
        if (!u.alive) { self.hold(0.5); return; }

        var mem = AI.getMem(u.id);
        var simT = AI.shared.simT;
        var prevT = mem.lastThinkT < 0 ? simT : mem.lastThinkT;
        var dt = Math.max(0.001, Math.min(0.2, simT - prevT));
        mem.lastThinkT = simT;

        AI.decayThreat(mem, dt);
        if (mem.shootCd > 0) mem.shootCd -= dt;
        for (var cd = 0; cd < mem.abCd.length; cd++) {
            if (mem.abCd[cd] > 0) mem.abCd[cd] -= dt;
        }

        var myTeam = u.teamId;
        var teammates = AI.shared.teams[myTeam];
        var enemies = AI.shared.teams[1 - myTeam];
        var teamFocus = AI.shared.teamFocus[myTeam];
        var nav = AI.shared.nav;
        var obstacles = AI.shared.obstacles;

        // Unstick: if steering nudged us onto a blocked cell, slide to the
        // nearest walkable neighbor. setTarget alone won't help — pathfinder
        // refuses to start on a blocked cell.
        if (nav && !nav.isWalkable(agent.x, agent.z)) {
            var free = AI.findWalkableNear(nav, agent.x, agent.z, 4);
            if (free) agent.setPosition(free.x, free.z);
        }

        // Nearest live enemy (for flee/threat).
        var closest = null, closestD2 = Infinity;
        for (var i = 0; i < enemies.length; i++) {
            var d2 = dist2(agent.x, agent.z, enemies[i].x, enemies[i].z);
            if (d2 < closestD2) { closestD2 = d2; closest = enemies[i]; }
        }

        // No enemies left — stand down.
        if (!closest) {
            mem.intent = "idle"; mem.targetId = null;
            BotAim.tick(mem.aim, dt);
            self.hold(0.25);
            return;
        }

        // Pick target for shooting + aim.
        var target = AI.pickTargetFor(agent, enemies, teamFocus, obstacles)
                   || teamFocus || closest;
        mem.targetId = target ? target.unit.id : null;

        // Request aim; tick the rotation. Aim is always toward the shot
        // target so we fire over-the-shoulder when kiting away.
        if (target) requestAimTowards(mem, simT, target.x - agent.x, target.z - agent.z);
        BotAim.tick(mem.aim, dt);

        // Side-effect: fire the basic shot if aim+cooldown+LOS align.
        if (target) tryAimedShot(agent, mem, target);

        var hpFrac = u.hp / u.maxHp;
        var tuning = AI.tuningFor(u.teamId);

        // Clear flee latch once HP is comfortable — next drop picks fresh.
        if (hpFrac >= tuning.fleeHpFrac + 0.1) { mem.fleeX = null; mem.fleeZ = null; }

        // ───── FLEE + HEAL when wounded ────────────────────────────────
        if (hpFrac < tuning.fleeHpFrac) {
            return fleeHeal(self, agent, mem, nav, closest, enemies, teammates, obstacles, simT);
        }

        // ───── HEAL ally ───────────────────────────────────────────────
        if (u.mana >= tuning.manaReserveHeal && mem.abCd[Arena.AB_HEAL] <= 0) {
            var wounded = null, worstHp = hpFrac;
            for (var ahi = 0; ahi < teammates.length; ahi++) {
                var at = teammates[ahi];
                if (at === agent) continue;
                var ahd = Math.hypot(at.x - agent.x, at.z - agent.z);
                if (ahd > 4) continue;
                var ahp = at.unit.hp / at.unit.maxHp;
                if (ahp < 0.75 && ahp < worstHp) { worstHp = ahp; wounded = at; }
            }
            if (wounded) {
                mem.intent = "HEAL_ALLY";
                doCast(self, mem, Arena.AB_HEAL, wounded.unit.id);
                return;
            }
        }

        // ───── SEEK COVER under fire ──────────────────────────────────
        var underFire = (simT - mem.lastHitT) < 2.0 && mem.threat > 8;
        if (underFire && hpFrac < 0.7) {
            var threat = null;
            for (var ti = 0; ti < enemies.length; ti++) {
                if (enemies[ti].unit.id === mem.threatSourceId) { threat = enemies[ti]; break; }
            }
            if (threat) {
                var needNew = mem.coverX === null
                    || (simT - mem.coverPickedT) > 0.4
                    || Math.hypot(agent.x - mem.coverX, agent.z - mem.coverZ) < 0.6;
                if (needNew) {
                    var cover = AI.findCover(agent, [threat], obstacles, nav,
                        { claimed: AI.claimedCover });
                    if (cover) {
                        mem.coverX = cover.x; mem.coverZ = cover.z;
                        mem.coverPickedT = simT;
                        AI.claimedCover.push(cover);
                    }
                }
                if (mem.coverX !== null) {
                    mem.intent = "COVER";
                    // Self-heal on the way to cover.
                    if (u.mana >= 25 && hpFrac < 0.65
                        && mem.abCd[Arena.AB_HEAL] <= 0) {
                        doCast(self, mem, Arena.AB_HEAL, u.id);
                        return;
                    }
                    self.moveTo(mem.coverX, mem.coverZ);
                    return;
                }
            }
        }
        if (hpFrac > 0.85 && (simT - mem.lastHitT) > 2.5) mem.coverX = null;

        // ───── ABILITIES (offensive) ──────────────────────────────────
        if (target && u.hp > 0) {
            var tdx = target.x - agent.x, tdz = target.z - agent.z;
            var tdist = Math.hypot(tdx, tdz);
            var tLos = bro.ai.game.hasLineOfSight(agent.x, agent.z, target.x, target.z, obstacles);

            // Grenade — cluster detection around target.
            if (tLos && u.mana >= 35 && tdist > 3 && tdist < 12
                && mem.abCd[Arena.AB_GRENADE] <= 0) {
                var cluster = 0;
                for (var c = 0; c < enemies.length; c++) {
                    if (dist2(enemies[c].x, enemies[c].z, target.x, target.z) < 9) cluster++;
                }
                if (cluster >= 2) {
                    mem.intent = "GRENADE";
                    doCast(self, mem, Arena.AB_GRENADE, target.unit.id);
                    return;
                }
            }
            // Beam — collinear enemy behind target.
            if (tLos && u.mana >= 30 && tdist > 2 && tdist < 16
                && mem.abCd[Arena.AB_BEAM] <= 0) {
                var mag1 = Math.max(0.01, tdist);
                for (var b = 0; b < enemies.length; b++) {
                    var be = enemies[b];
                    if (be === target) continue;
                    var bx = be.x - agent.x, bz = be.z - agent.z;
                    var mag2 = Math.hypot(bx, bz);
                    if (mag2 < 0.01 || mag2 <= mag1) continue;
                    var cos = (tdx * bx + tdz * bz) / (mag1 * mag2);
                    if (cos > 0.97) {
                        mem.intent = "BEAM";
                        doCast(self, mem, Arena.AB_BEAM, target.unit.id);
                        return;
                    }
                }
            }
            // Fireball — mid-range poke. Gated on tuning-specified HP
            // comfort so aggro scripts spam while defensive/kite hold mana.
            if (tLos && u.mana >= 20 && tdist > 4 && tdist < 13
                && mem.abCd[Arena.AB_FIREBALL] <= 0
                && hpFrac >= tuning.fireballMinHp
                && Math.random() < 0.06) {
                mem.intent = "FIREBALL";
                doCast(self, mem, Arena.AB_FIREBALL, target.unit.id);
                return;
            }
        }

        // ───── FIRING BAND (engage / kite / hold) ─────────────────────
        if (!target) { mem.intent = "idle"; self.hold(0.2); return; }
        var dx2 = target.x - agent.x, dz2 = target.z - agent.z;
        var dist = Math.hypot(dx2, dz2);
        var range = u.attackRange;
        var hasLOS = bro.ai.game.hasLineOfSight(
            agent.x, agent.z, target.x, target.z, obstacles);

        var isSupport = hpFrac < 0.75;
        mem.role = isSupport ? "support" : "front";
        var tooFar  = range * (isSupport ? tuning.supportEngageDistMul : tuning.engageDistMul);
        var tooNear = range * (isSupport ? tuning.supportKiteDistMul   : tuning.kiteDistMul);

        if (!hasLOS) {
            mem.intent = "REPOSITION";
            var rx = target.x, rz = target.z;
            if (nav && !nav.isWalkable(rx, rz)) {
                var w = AI.findWalkableNear(nav, rx, rz, 2);
                if (w) { rx = w.x; rz = w.z; }
            }
            var rSp = AI.spaceFromTeammates(agent, teammates, rx, rz);
            self.moveTo(rSp.x, rSp.z); return;
        }
        if (dist > tooFar) {
            mem.intent = "ENGAGE";
            var eSp = AI.spaceFromTeammates(agent, teammates, target.x, target.z);
            self.moveTo(eSp.x, eSp.z); return;
        }
        if (dist < tooNear) {
            mem.intent = "KITE";
            var n = Math.max(0.01, dist);
            var bx2 = agent.x - (dx2 / n) * 2.0;
            var bz2 = agent.z - (dz2 / n) * 2.0;
            var kSp = AI.spaceFromTeammates(agent, teammates, bx2, bz2);
            if (nav && !nav.isWalkable(kSp.x, kSp.z)) { self.hold(0.1); return; }
            self.moveTo(kSp.x, kSp.z); return;
        }
        // In band — strafe/hold and keep shooting (side-effect above).
        mem.intent = "FIRE";
        if (simT - mem.lastFlip > 0.8) { mem.perpSign = -mem.perpSign; mem.lastFlip = simT; }
        var norm2 = Math.max(0.01, dist);
        var sx = agent.x + (-dz2 / norm2) * mem.perpSign * 1.2;
        var sz = agent.z + ( dx2 / norm2) * mem.perpSign * 1.2;
        var fSp = AI.spaceFromTeammates(agent, teammates, sx, sz);
        if (nav && nav.isWalkable(fSp.x, fSp.z)) self.moveTo(fSp.x, fSp.z);
        else self.hold(0.1);
    };

    // ───────────────────────────────────────────────────────────────────────
    // FLEE + HEAL branch — wounded units rally to a healthy ally, self-heal,
    // and keep firing back on the way. Extracted for readability.
    // ───────────────────────────────────────────────────────────────────────
    function fleeHeal(self, agent, mem, nav, closest, enemies, teammates, obstacles, simT) {
        var u = agent.unit;
        mem.intent = "FLEE";
        // Self-heal takes priority over movement if available.
        if (u.mana >= 25 && mem.abCd[Arena.AB_HEAL] <= 0) {
            mem.intent = "HEAL";
            doCast(self, mem, Arena.AB_HEAL, u.id);
            return;
        }

        // Pick a rally ally — healthy + out of enemy range preferred.
        var rally = null, rallyScore = -Infinity, tcx = 0, tcz = 0, tcN = 0;
        for (var mi = 0; mi < teammates.length; mi++) {
            var mt = teammates[mi];
            if (mt === agent) continue;
            tcx += mt.x; tcz += mt.z; tcN++;
            var mHp = mt.unit.hp / mt.unit.maxHp;
            if (mHp < 0.5) continue;
            var nearestE = Infinity;
            for (var mei = 0; mei < enemies.length; mei++) {
                var mEd = Math.hypot(enemies[mei].x - mt.x, enemies[mei].z - mt.z);
                if (mEd < nearestE) nearestE = mEd;
            }
            var safeR = (u.attackRange || 9) * 1.1;
            var safeBonus = nearestE > safeR ? 60 : 0;
            var md = Math.hypot(mt.x - agent.x, mt.z - agent.z);
            var score = mHp * 100 - md + safeBonus;
            if (score > rallyScore) { rallyScore = score; rally = mt; }
        }

        // Latch the flee destination so the whole squad doesn't chase a
        // moving "safe spot" and jitter. Re-pick when enemy closes on it
        // or after 3s stale.
        var cachedStillGood = mem.fleeX != null;
        if (cachedStillGood) {
            for (var ci3 = 0; ci3 < enemies.length; ci3++) {
                var dE = Math.hypot(enemies[ci3].x - mem.fleeX, enemies[ci3].z - mem.fleeZ);
                if (dE < (enemies[ci3].unit.attackRange || 9)) { cachedStillGood = false; break; }
            }
            if ((simT - (mem.fleePickedT || -99)) > 3.0) cachedStillGood = false;
        }

        var fx = agent.x, fz = agent.z, foundDest = false;
        if (cachedStillGood) {
            fx = mem.fleeX; fz = mem.fleeZ; foundDest = true;
            AI.claimedCover.push({ x: fx, z: fz });
        }
        if (!foundDest && rally) {
            // Anchor 1.8u past the ally away from the threat — ally
            // between us and the shooter.
            var thx = closest.x, thz = closest.z;
            var avx = rally.x - thx, avz = rally.z - thz;
            var avm = Math.max(0.01, Math.hypot(avx, avz));
            var anchorX = rally.x + (avx / avm) * 1.8;
            var anchorZ = rally.z + (avz / avm) * 1.8;
            var flCover = AI.findCover(agent, [closest], obstacles, nav,
                { anchorX: anchorX, anchorZ: anchorZ, claimed: AI.claimedCover });
            if (flCover) {
                fx = flCover.x; fz = flCover.z;
                AI.claimedCover.push(flCover);
                foundDest = true;
            }
        }
        if (!foundDest) {
            var rx, rz;
            if (rally) {
                var thx2 = closest.x, thz2 = closest.z;
                var avx2 = rally.x - thx2, avz2 = rally.z - thz2;
                var avm2 = Math.max(0.01, Math.hypot(avx2, avz2));
                rx = rally.x + (avx2 / avm2) * 1.8;
                rz = rally.z + (avz2 / avm2) * 1.8;
            } else if (tcN > 0) {
                var cx3 = tcx / tcN, cz3 = tcz / tcN;
                var slotAng = (u.id * 2.39996) % (Math.PI * 2);
                rx = cx3 + Math.cos(slotAng) * 2.5;
                rz = cz3 + Math.sin(slotAng) * 2.5;
            } else {
                rx = agent.x + (agent.x - closest.x);
                rz = agent.z + (agent.z - closest.z);
            }
            for (var ci2 = 0; ci2 < AI.claimedCover.length; ci2++) {
                var cl2 = AI.claimedCover[ci2];
                var ddx = rx - cl2.x, ddz = rz - cl2.z;
                var dd = Math.hypot(ddx, ddz);
                if (dd < 1.2 && dd > 0.01) {
                    rx += (ddx / dd) * (1.2 - dd);
                    rz += (ddz / dd) * (1.2 - dd);
                }
            }
            fx = clamp(rx, -19, 19); fz = clamp(rz, -19, 19);
            if (nav && !nav.isWalkable(fx, fz)) {
                var fw = AI.findWalkableNear(nav, fx, fz, 3);
                if (fw) { fx = fw.x; fz = fw.z; }
            }
            AI.claimedCover.push({ x: fx, z: fz });
        }
        if (!cachedStillGood) {
            var safe = AI.pushOutOfEnemyRange(fx, fz, enemies, 1.15);
            if (nav && !nav.isWalkable(safe.x, safe.z)) {
                var safeW = AI.findWalkableNear(nav, safe.x, safe.z, 3);
                if (safeW) { fx = safeW.x; fz = safeW.z; }
            } else { fx = safe.x; fz = safe.z; }
            var spaced = AI.spaceFromTeammates(agent, teammates, fx, fz);
            fx = spaced.x; fz = spaced.z;
            if (nav && !nav.isWalkable(fx, fz)) {
                var sw2 = AI.findWalkableNear(nav, fx, fz, 3);
                if (sw2) { fx = sw2.x; fz = sw2.z; }
            }
            mem.fleeX = fx; mem.fleeZ = fz; mem.fleePickedT = simT;
        }
        self.moveTo(fx, fz);
    }
})();
