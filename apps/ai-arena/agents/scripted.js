// agents/scripted.js — Hand-tuned baseline agent, expressed as a Bot-command
// planner.
//
// Where this used to just delegate to AI.think (which issued self.moveTo /
// self.cast directly), it now builds a single Bot *command* per tick that
// describes the hero's current intent, pushes it into the Bot queue, and
// lets Bot.tick handle execution. The decision logic still lives here
// (priority ladder: flee / heal-ally / cover / abilities / engagement band);
// we just stop dispatching movement + firing ourselves and hand both over
// to the shared reflex robot.
//
// Why: the same robot runs the MCTS/commander agents, so scripted
// automatically picks up any future improvement to target selection,
// aim-delay, cover-seeking or ability autocast. It also lets every agent
// be measured apples-to-apples — differences in winrate now reflect
// planner quality, not execution quality.
//
// The AI.* helpers (pickTargetFor, findCover, spaceFromTeammates,
// chooseTeamFocus) remain in ai.js and are reused here. AI.updateShared
// is still called each frame from loop.js so the shared view of the world
// is populated before any agent thinks.

(function () {
    "use strict";

    function hpFrac(u) { return u.maxHp > 0 ? u.hp / u.maxHp : 0; }

    function collinearExists(agent, target, enemies) {
        var tdx = target.x - agent.x, tdz = target.z - agent.z;
        var tdist = Math.hypot(tdx, tdz);
        if (tdist < 0.01) return false;
        for (var b = 0; b < enemies.length; b++) {
            var be = enemies[b];
            if (be === target || !be.unit.alive) continue;
            var bx = be.x - agent.x, bz = be.z - agent.z;
            var bm = Math.hypot(bx, bz);
            if (bm < 0.01 || bm <= tdist) continue;
            if ((tdx * bx + tdz * bz) / (tdist * bm) > 0.97) return true;
        }
        return false;
    }

    function nearestEnemy(agent, enemies) {
        var best = null, bestD = Infinity;
        for (var i = 0; i < enemies.length; i++) {
            var e = enemies[i];
            if (!e.unit.alive) continue;
            var d = (e.x - agent.x) * (e.x - agent.x) + (e.z - agent.z) * (e.z - agent.z);
            if (d < bestD) { bestD = d; best = e; }
        }
        return best;
    }

    function findWoundedAlly(agent, teammates) {
        var best = null, worstHp = hpFrac(agent.unit);
        for (var ai = 0; ai < teammates.length; ai++) {
            var at = teammates[ai];
            if (at === agent || !at.unit.alive) continue;
            var ahd = Math.hypot(at.x - agent.x, at.z - agent.z);
            if (ahd > 4) continue;       // heal range ~= 4
            var ahp = hpFrac(at.unit);
            if (ahp < 0.75 && ahp < worstHp) { worstHp = ahp; best = at; }
        }
        return best;
    }

    // Build the command that encodes scripted's current intent. Priority
    // ladder is the same as ai.js AI.think; we just translate the tail of
    // each branch (what it would have called self.*) into a robot command.
    function makeCommand(self) {
        var agent = self.agent;
        var u = agent.unit;
        var mem = AI.getMem(u.id);
        var tuning = AI.tuningFor(u.teamId);
        var enemies = AI.shared.teams[1 - u.teamId] || [];
        var teammates = AI.shared.teams[u.teamId] || [];
        var obstacles = AI.shared.obstacles || [];
        var simT = AI.shared.simT;
        var closest = nearestEnemy(agent, enemies);
        var hpF = hpFrac(u);

        if (!closest) {
            return {
                target: { policy: "nearest" },
                fireBasic: false,
                move: { mode: "hold" },
            };
        }

        // FLEE + self-heal on the way. The cover-from move picks a
        // LOS-breaking cell via AI.findCover inside Bot; self-heal fires
        // as an autocast when off-cooldown + mana.
        if (hpF < tuning.fleeHpFrac) {
            return {
                target: { policy: "nearest", requireLOS: false },
                fireBasic: true,                     // over-the-shoulder shots
                allowAbilities: {
                    0: { target: "self", minMana: 25 },
                },
                move: { mode: "coverFrom",
                        fromId: closest.unit.id, space: true },
            };
        }

        // HEAL_ALLY — heal the most-wounded ally within range. Aim gets
        // pointed at the ally so the cast's cfg.target="focus" resolves
        // to their id; basic firing suppressed to avoid friendly-fire
        // projectiles that would just fly past.
        if (u.mana >= tuning.manaReserveHeal && (mem.abCd || [0])[0] <= 0) {
            var wounded = findWoundedAlly(agent, teammates);
            if (wounded) {
                return {
                    target: { policy: "id", id: wounded.unit.id, requireLOS: false },
                    fireBasic: false,
                    allowAbilities: {
                        0: { target: "focus", minMana: 25 },
                    },
                    move: { mode: "hold" },
                };
            }
        }

        // SEEK COVER under fire. Scripted latches on mem.threatSourceId;
        // we pass that id to Bot's coverFrom so it finds a cell that
        // breaks LOS from the specific shooter, not just the nearest
        // enemy.
        var underFire = (simT - mem.lastHitT) < 2.0 && mem.threat > 8;
        if (underFire && hpF < 0.7 && mem.threatSourceId >= 0) {
            return {
                target: { policy: "nearest", requireLOS: false },
                fireBasic: true,
                allowAbilities: {
                    0: { target: "self", minMana: 25, maxHpFrac: 0.65 },
                },
                move: { mode: "coverFrom",
                        fromId: mem.threatSourceId, space: true },
            };
        }

        // ENGAGEMENT. Pick the best target (hp + range + LOS weighted)
        // and emit a context-appropriate movement + full offensive
        // ability loadout. Bot evaluates the ability gates every tick
        // while we're committed to this command so we don't miss a
        // window because the planner happened to replan late.
        var target = AI.pickTargetFor(agent, enemies,
            AI.shared.teamFocus[u.teamId], obstacles) || closest;

        var dx = target.x - agent.x, dz = target.z - agent.z;
        var d = Math.hypot(dx, dz);
        var range = u.attackRange;
        var hasLOS = bro.ai.game.hasLineOfSight(
            agent.x, agent.z, target.x, target.z, obstacles);
        var isSupport = hpF < 0.75;
        var kiteLo = isSupport ? tuning.supportKiteDistMul   : tuning.kiteDistMul;
        var kiteHi = isSupport ? tuning.supportEngageDistMul : tuning.engageDistMul;

        var move;
        if (!hasLOS) {
            // Reposition toward the target to reacquire.
            move = { mode: "moveTo", x: target.x, z: target.z, space: true };
        } else if (d > range * kiteHi) {
            move = { mode: "advance", kiteBand: [kiteLo, kiteHi], space: true };
        } else if (d < range * kiteLo) {
            move = { mode: "retreat", fromId: target.unit.id, space: true };
        } else {
            move = { mode: "strafe", kiteBand: [kiteLo, kiteHi], space: true };
        }

        var allow = {};
        // Grenade — only when a cluster exists around any enemy.
        if (u.mana >= 35 && (mem.abCd || [0,0,0,0])[3] <= 0) {
            allow[3] = { target: "cluster", minMana: 35,
                clusterRadius: 3.0, minCluster: 2 };
        }
        // Beam — collinear enemy behind the current target. The gate
        // runs every Bot.maybeCast pass, so a passing alignment triggers
        // the cast even mid-queue.
        if (u.mana >= 30 && (mem.abCd || [0,0,0])[2] <= 0) {
            allow[2] = {
                target: "focus", minMana: 30,
                gate: function (gctx) {
                    var a = gctx.agent, t = gctx.target;
                    if (!t) return false;
                    var dd = Math.hypot(t.x - a.x, t.z - a.z);
                    if (dd <= 2 || dd >= 16) return false;
                    return collinearExists(a, t, enemies);
                },
            };
        }
        // Fireball — probabilistic poke, mirrors scripted's 6%-per-tick
        // chance. Gate sees the picked target from maybeCast ctx.
        if (u.mana >= 20 && hpF >= tuning.fireballMinHp
            && (mem.abCd || [0,0])[1] <= 0) {
            allow[1] = {
                target: "focus", minMana: 20,
                gate: function (gctx) {
                    var a = gctx.agent, t = gctx.target;
                    if (!t) return false;
                    var dd = Math.hypot(t.x - a.x, t.z - a.z);
                    if (dd <= 4 || dd >= 13) return false;
                    return Math.random() < 0.06;
                },
            };
        }

        return {
            target: { policy: "id", id: target.unit.id, requireLOS: false },
            fireBasic: true,
            allowAbilities: allow,
            move: move,
        };
    }

    Agents.register({
        id: "scripted",
        label: "Scripted",
        think: function (self /*, world*/) {
            var u = self.agent.unit;
            if (!u.alive) { self.hold(0.5); return; }
            var mem = AI.getMem(u.id);
            var simT = AI.shared.simT;
            var prevT = mem.lastThinkT < 0 ? simT : mem.lastThinkT;
            var dt = Math.max(0.001, Math.min(0.2, simT - prevT));
            mem.lastThinkT = simT;

            // Scripted is fully reactive: replace the queue every tick
            // with the command for the current state. Short duration (a
            // single think window) so Bot.tick's queue bookkeeping treats
            // each entry as "this tick's intent".
            var cmd = makeCommand(self);
            Bot.replace(self, [{ cmd: cmd, duration: 0.2 }]);
            Bot.tick(self, dt);
        },
    });
})();
