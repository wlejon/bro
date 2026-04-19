// bot.js — Reflex robot that handles the low-level fundamentals shared by
// every agent in ai-arena: target selection, aim (BotAim turn-rate limit),
// basic-shot firing when aligned, movement with spacing/cover, and optional
// ability autocast. Planners (scripted, options_mcts, options_commander) do
// NOT drive the robot directly — they push commands into the robot's queue
// and the robot runs itself, consuming one command at a time until it
// expires.
//
// This decoupling matters for MCTS-style planners: instead of producing one
// command per tick (reactive), a planner can project forward and queue a
// sequence ("advance 1.5s, then strafe 2s, then fireball"). The robot
// executes autonomously — the planner can even skip ticks. Reactive agents
// (scripted) push one short-duration command per tick and the queue stays
// near-empty; deliberative agents (MCTS) push longer-running commands and
// let the robot handle execution.
//
// The robot lives in JS and drives the LIVE game via self.moveTo / direct
// projectile spawn — same pattern as ai.js::tryAimedShot. The MCTS internal
// sim does its own simulation; the robot is orthogonal to it. All that's
// needed for MCTS parity is that the basic-shot ability's fn is snapshot
// safe (uses caster.aimYaw, not JS BotAim state) — done in arena.js.
//
// Command schema:
//
//   {
//     // Target selection (required for firing)
//     target: {
//       policy: "focus" | "nearest" | "weakest" | "teamFocus" | "id",
//       id: <unitId>,           // when policy === "id"
//       maxRange: 18,           // cap on consideration distance; default 18
//       requireLOS: true,       // default true
//     },
//
//     // Firing gates
//     fireBasic: true,          // enable basic-shot side-effect
//     allowAbilities: {         // keyed by slot index; absent = not autofired
//       [slot]: {
//         gate: (ctx) => bool,       // optional extra condition
//         target: "focus"|"self"|"cluster", // who to target (default "focus")
//         minMana: 0, minHpFrac: 0,  // gate shorthands
//       },
//     },
//
//     // Movement mode — pick one
//     move: {
//       mode: "hold"|"moveTo"|"advance"|"retreat"|"coverFrom"|"strafe"|"rally",
//       x, z,                   // for "moveTo"
//       space: true,            // apply spaceFromTeammates (default true)
//       kiteBand: [0.45, 0.85], // for strafe/advance/retreat; fraction of range
//     },
//   }
//
// Return value: nothing. The robot calls self.moveTo / self.hold directly.

var Bot = (function () {
    "use strict";

    function dist2(x1, z1, x2, z2) {
        var dx = x1 - x2, dz = z1 - z2;
        return dx * dx + dz * dz;
    }
    function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

    // ── Target selection ──────────────────────────────────────────────────
    //
    // Returns a bound Agent (or null). The result latches on mem.lastTarget
    // for one tick to avoid aim-thrash when policies flip between close
    // candidates. Caller's policy still wins next tick — we only stabilize
    // within a single decision.
    function pickTarget(agent, mem, cmd, ctx) {
        var t = cmd.target || {};
        var maxRange = t.maxRange || 18;
        var requireLOS = t.requireLOS !== false;
        var enemies = ctx.enemies;
        var obstacles = ctx.obstacles;

        function inRange(e) {
            return dist2(agent.x, agent.z, e.x, e.z) <= maxRange * maxRange;
        }
        function hasLOS(e) {
            return !requireLOS || bro.ai.game.hasLineOfSight(
                agent.x, agent.z, e.x, e.z, obstacles);
        }

        var pick = null;
        if (t.policy === "id" && t.id !== undefined) {
            for (var i = 0; i < enemies.length; i++) {
                if (enemies[i].unit.id === t.id && enemies[i].unit.alive) {
                    pick = enemies[i]; break;
                }
            }
        } else if (t.policy === "teamFocus") {
            pick = ctx.teamFocus;
        } else if (t.policy === "weakest") {
            var bestHp = Infinity;
            for (var wi = 0; wi < enemies.length; wi++) {
                var e = enemies[wi];
                if (!inRange(e) || !hasLOS(e)) continue;
                if (e.unit.hp < bestHp) { bestHp = e.unit.hp; pick = e; }
            }
        } else { // "nearest" | "focus" | default
            var bestD = Infinity;
            for (var ni = 0; ni < enemies.length; ni++) {
                var en = enemies[ni];
                if (!hasLOS(en)) continue;
                var d = dist2(agent.x, agent.z, en.x, en.z);
                if (d > maxRange * maxRange) continue;
                // "focus" prefers lower HP at similar distance
                var score = (t.policy === "focus")
                    ? d + en.unit.hp * 0.5
                    : d;
                if (score < bestD) { bestD = score; pick = en; }
            }
        }

        // Fallback: raw-nearest enemy ignoring LOS (so BotAim keeps pointing
        // somewhere reasonable even when cover breaks the shot).
        if (!pick) {
            var fbD = Infinity;
            for (var fi = 0; fi < enemies.length; fi++) {
                var fe = enemies[fi];
                var fd = dist2(agent.x, agent.z, fe.x, fe.z);
                if (fd < fbD) { fbD = fd; pick = fe; }
            }
        }
        return pick;
    }

    // ── Aim + basic-shot firing ──────────────────────────────────────────
    //
    // Updates BotAim (reaction lag + turn rate) toward the picked target and
    // spawns a basic-shot projectile when aim / cooldown / LOS align. Mirror
    // of ai.js::requestAimTowards + tryAimedShot so the robot's firing
    // cadence matches scripted's.
    function handleAim(agent, mem, target, cmd, ctx, dt) {
        if (!mem.aim) return;
        var simT = ctx.simT;

        // Free-look yaw overrides target-based aim. Used by the random
        // agent to scan the arena independently of any enemy target.
        if (cmd.lookYaw !== undefined && cmd.lookYaw !== null) {
            BotAim.requestAim(mem.aim, simT, cmd.lookYaw, 0);
        } else if (target) {
            var dx = target.x - agent.x, dz = target.z - agent.z;
            var yaw = Math.atan2(dx, -dz);
            BotAim.requestAim(mem.aim, simT, yaw, 0);
        }
        BotAim.tick(mem.aim, dt);

        // Decay our JS-side basic-shot cooldown.
        if (mem.shootCd > 0) mem.shootCd = Math.max(0, mem.shootCd - dt);

        if (!cmd.fireBasic || !target) return;
        if (mem.shootCd > 0) return;
        if (!target.unit.alive) return;

        var u = agent.unit;
        var tdx = target.x - agent.x, tdz = target.z - agent.z;
        var td = Math.sqrt(tdx * tdx + tdz * tdz);
        if (td > u.attackRange) return;
        if (!bro.ai.game.hasLineOfSight(
            agent.x, agent.z, target.x, target.z, ctx.obstacles)) return;
        if (!BotAim.canFireAt(mem.aim, agent.x, 0, agent.z,
            target.x, 0, target.z)) return;

        var f = BotAim.forward(mem.aim);
        var PSPEED = 18;
        ctx.world.spawnProjectile({
            ownerId: u.id, teamId: u.teamId,
            x: agent.x + f.x * (u.radius + 0.4),
            z: agent.z + f.z * (u.radius + 0.4),
            vx: f.x * PSPEED, vz: f.z * PSPEED,
            speed: PSPEED, radius: 0.22,
            damage: 9, remainingLife: 1.2,
            kind: "physical", mode: "single",
        });
        mem.shootCd = 1.0 / Math.max(0.1, u.attacksPerSec || 1.4);
    }

    // ── Ability autocast ─────────────────────────────────────────────────
    //
    // Returns true if a cast was queued (caller should skip movement this
    // tick — `self.cast` clobbers the pending Action so a subsequent moveTo
    // would overwrite it). Iterates allowAbilities in declaration order and
    // fires the first one whose gates pass.
    function maybeCast(self, agent, mem, target, cmd, ctx) {
        var allow = cmd.allowAbilities;
        if (!allow) return false;
        var u = agent.unit;

        function abReady(slot) {
            var c = mem.abCd ? mem.abCd[slot] : 0;
            return (c || 0) <= 0;
        }

        // Cluster head: enemy with the most neighbors within cluster radius.
        function clusterHead(radius) {
            var enemies = ctx.enemies;
            var best = null, bestN = 0;
            for (var i = 0; i < enemies.length; i++) {
                var n = 0;
                for (var j = 0; j < enemies.length; j++) {
                    if (dist2(enemies[i].x, enemies[i].z,
                        enemies[j].x, enemies[j].z) <= radius * radius) n++;
                }
                if (n > bestN) { bestN = n; best = enemies[i]; }
            }
            return { head: best, count: bestN };
        }

        for (var slot in allow) {
            if (!Object.prototype.hasOwnProperty.call(allow, slot)) continue;
            var s = parseInt(slot, 10);
            var cfg = allow[slot];
            if (!abReady(s)) continue;
            if (cfg.minMana !== undefined && u.mana < cfg.minMana) continue;
            var hpF = u.maxHp > 0 ? u.hp / u.maxHp : 0;
            if (cfg.minHpFrac !== undefined && hpF < cfg.minHpFrac) continue;
            if (cfg.maxHpFrac !== undefined && hpF > cfg.maxHpFrac) continue;

            var tid = u.id;
            var aimTarget = target;
            if (cfg.target === "self") {
                tid = u.id;
            } else if (cfg.target === "cluster") {
                var cr = cfg.clusterRadius || 2.5;
                var minC = cfg.minCluster || 2;
                var c = clusterHead(cr);
                if (!c.head || c.count < minC) continue;
                aimTarget = c.head;
                tid = c.head.unit.id;
            } else {
                if (!target) continue;
                tid = target.unit.id;
            }

            if (cfg.gate && !cfg.gate({ agent: agent, target: aimTarget, ctx: ctx })) continue;

            self.cast(s, tid);
            // Seed the mirror cd so we don't re-fire this slot next tick.
            if (mem.abCd && ctx.abilityCooldowns && ctx.abilityCooldowns[s]) {
                mem.abCd[s] = ctx.abilityCooldowns[s];
            } else if (mem.abCd) {
                mem.abCd[s] = cfg.cooldown || 1;
            }
            return true;
        }
        return false;
    }

    // ── Movement ─────────────────────────────────────────────────────────
    //
    // Translates the move command into a self.moveTo / self.hold call.
    // Applies spacing + cover logic lifted from ai.js so execution quality
    // matches scripted's baseline.
    function handleMove(self, agent, mem, target, cmd, ctx) {
        var m = cmd.move || { mode: "hold" };
        var u = agent.unit;
        var range = u.attackRange || 9;
        var kite = m.kiteBand || [0.45, 0.85];
        var tooNear = range * kite[0];
        var tooFar  = range * kite[1];
        var space = m.space !== false;
        var fx = agent.x, fz = agent.z;
        var mode = m.mode || "hold";

        if (mode === "hold") {
            self.hold(0.2); return;
        }

        if (mode === "moveTo") {
            fx = m.x; fz = m.z;
        } else if (mode === "advance") {
            if (!target) { self.hold(0.15); return; }
            var ax = target.x - agent.x, az = target.z - agent.z;
            var ad = Math.max(0.01, Math.hypot(ax, az));
            if (ad <= tooFar) { // already in band — strafe
                mode = "strafe";
            } else {
                // Step toward target
                fx = target.x; fz = target.z;
            }
        } else if (mode === "retreat") {
            var threat = m.fromId
                ? findEnemyById(ctx.enemies, m.fromId)
                : (target || ctx.enemies[0]);
            if (!threat) { self.hold(0.1); return; }
            var dx = agent.x - threat.x, dz = agent.z - threat.z;
            var d  = Math.max(0.01, Math.hypot(dx, dz));
            fx = agent.x + (dx / d) * range;
            fz = agent.z + (dz / d) * range;
        } else if (mode === "coverFrom") {
            var cfThreat = m.fromId
                ? findEnemyById(ctx.enemies, m.fromId)
                : target;
            if (cfThreat && ctx.nav && AI.findCover) {
                var cov = AI.findCover(agent, [cfThreat],
                    ctx.obstacles, ctx.nav, { claimed: AI.claimedCover });
                if (cov) {
                    fx = cov.x; fz = cov.z;
                    AI.claimedCover.push(cov);
                } else {
                    // No cover found — fall back to retreat
                    var rdx = agent.x - cfThreat.x, rdz = agent.z - cfThreat.z;
                    var rd  = Math.max(0.01, Math.hypot(rdx, rdz));
                    fx = agent.x + (rdx / rd) * range;
                    fz = agent.z + (rdz / rd) * range;
                }
            } else if (cfThreat) {
                var rdx2 = agent.x - cfThreat.x, rdz2 = agent.z - cfThreat.z;
                var rd2  = Math.max(0.01, Math.hypot(rdx2, rdz2));
                fx = agent.x + (rdx2 / rd2) * range;
                fz = agent.z + (rdz2 / rd2) * range;
            }
        }

        if (mode === "strafe") {
            if (!target) { self.hold(0.15); return; }
            var simT = ctx.simT;
            if (mem.lastFlip === undefined || simT - mem.lastFlip > 0.8) {
                mem.perpSign = -(mem.perpSign || 1);
                mem.lastFlip = simT;
            }
            var sdx = target.x - agent.x, sdz = target.z - agent.z;
            var sn = Math.max(0.01, Math.hypot(sdx, sdz));
            // Perpendicular step
            var px = agent.x + (-sdz / sn) * (mem.perpSign || 1) * 1.2;
            var pz = agent.z + ( sdx / sn) * (mem.perpSign || 1) * 1.2;
            // Pull in/out to stay in kite band
            if (sn > tooFar) {
                px += (sdx / sn) * (sn - tooFar);
                pz += (sdz / sn) * (sn - tooFar);
            } else if (sn < tooNear) {
                px -= (sdx / sn) * (tooNear - sn);
                pz -= (sdz / sn) * (tooNear - sn);
            }
            fx = px; fz = pz;
        }

        // Spacing + nav fixup.
        if (space && AI.spaceFromTeammates) {
            var sp = AI.spaceFromTeammates(agent, ctx.teammates, fx, fz);
            fx = sp.x; fz = sp.z;
        }
        if (ctx.nav && !ctx.nav.isWalkable(fx, fz) && AI.findWalkableNear) {
            var fw = AI.findWalkableNear(ctx.nav, fx, fz, 3);
            if (fw) { fx = fw.x; fz = fw.z; }
        }
        fx = clamp(fx, -19, 19); fz = clamp(fz, -19, 19);
        self.moveTo(fx, fz);
    }

    function findEnemyById(list, id) {
        for (var i = 0; i < list.length; i++) {
            if (list[i].unit.id === id) return list[i];
        }
        return null;
    }

    // ── Per-channel command queues ───────────────────────────────────────
    //
    // The robot exposes four INDEPENDENT queues — move, aim, fire, cast —
    // each holding up to MAX_QUEUE entries. Channels advance on their own
    // clocks: a 2-second "advance" move can run alongside a sequence of
    // 0.3-second look-yaw scans and a steady "fire when aligned" policy,
    // because they live in separate queues. This lets the random agent
    // (and future planners) compose look + walk + shoot the way a real
    // controller would — three independent thumb/finger streams, not one
    // bundled command.
    //
    // Entry shape per channel: { partial, remaining }. partial is the
    // channel-specific slice of the bundled cmd schema:
    //
    //   move.partial  = { mode, x, z, fromId, kiteBand, space, ... }
    //   aim.partial   = { policy, id, maxRange, requireLOS } | { lookYaw }
    //   fire.partial  = boolean | { enabled }
    //   cast.partial  = { [slot]: { gate, target, minMana, ... }, ... }
    //
    // Per tick we pull the head of each queue, decrement its `remaining`,
    // pop on expiry, then synthesize one bundled cmd and feed it through
    // the existing handleAim/handleMove/maybeCast paths — which already
    // do aim+fire as side effects alongside the single move-or-cast pick.
    //
    // The legacy bundled API (Bot.push/replace with `{cmd, duration}`)
    // still works: it fans the cmd out into all four channels with the
    // same duration. Existing planners (scripted, options_mcts,
    // options_commander) keep working unchanged.

    var DEFAULT_DURATION = 0.2;
    var MAX_QUEUE = 10;
    var CHANNELS = ["move", "aim", "fire", "cast"];

    function queuesOf(agent) {
        var mem = AI.getMem(agent.unit.id);
        if (!mem.botChannels) {
            mem.botChannels = { move: [], aim: [], fire: [], cast: [] };
        }
        return mem.botChannels;
    }

    function partialFromCmd(cmd, channel) {
        if (!cmd) return null;
        if (channel === "move") return cmd.move || null;
        if (channel === "aim") {
            if (cmd.lookYaw !== undefined) return { lookYaw: cmd.lookYaw };
            return cmd.target || null;
        }
        if (channel === "fire") {
            if (cmd.fireBasic === undefined) return null;
            return !!cmd.fireBasic;
        }
        if (channel === "cast") return cmd.allowAbilities || null;
        return null;
    }

    function pushChannel(self, channel, partial, durationSec) {
        var q = queuesOf(self.agent)[channel];
        if (!q) return;
        if (q.length >= MAX_QUEUE) q.shift();   // newest wins when saturated
        q.push({ partial: partial, remaining: durationSec !== undefined
            ? durationSec : DEFAULT_DURATION });
    }

    function replaceChannel(self, channel, entries) {
        var q = queuesOf(self.agent)[channel];
        if (!q) return;
        q.length = 0;
        if (!entries) return;
        if (!Array.isArray(entries)) entries = [entries];
        for (var i = 0; i < entries.length && q.length < MAX_QUEUE; i++) {
            var e = entries[i];
            if (e && Object.prototype.hasOwnProperty.call(e, "partial")) {
                q.push({ partial: e.partial, remaining: e.duration !== undefined
                    ? e.duration : DEFAULT_DURATION });
            } else {
                q.push({ partial: e, remaining: DEFAULT_DURATION });
            }
        }
    }

    function clearChannel(self, channel) {
        var q = queuesOf(self.agent)[channel];
        if (q) q.length = 0;
    }

    function channelLength(self, channel) {
        var q = queuesOf(self.agent)[channel];
        return q ? q.length : 0;
    }

    // Legacy bundled API — fans a cmd out across all four channels.
    function push(self, cmd, durationSec) {
        for (var i = 0; i < CHANNELS.length; i++) {
            var ch = CHANNELS[i];
            var p = partialFromCmd(cmd, ch);
            if (p !== null) pushChannel(self, ch, p, durationSec);
        }
    }

    function replace(self, plan) {
        // Clear all channels, then re-queue from the bundled plan.
        for (var c = 0; c < CHANNELS.length; c++) clearChannel(self, CHANNELS[c]);
        if (!plan) return;
        if (!Array.isArray(plan)) plan = [plan];
        for (var i = 0; i < plan.length; i++) {
            var e = plan[i];
            var cmd = (e && e.cmd) ? e.cmd : e;
            var dur = (e && e.duration !== undefined) ? e.duration : DEFAULT_DURATION;
            for (var j = 0; j < CHANNELS.length; j++) {
                var ch = CHANNELS[j];
                var p = partialFromCmd(cmd, ch);
                if (p !== null) pushChannel(self, ch, p, dur);
            }
        }
    }

    function clear(self) {
        for (var i = 0; i < CHANNELS.length; i++) clearChannel(self, CHANNELS[i]);
    }

    function advanceChannelHead(q, dt) {
        while (q.length > 0) {
            q[0].remaining -= dt;
            if (q[0].remaining > 0) return q[0].partial;
            q.shift();
        }
        return null;
    }

    // Synthesize the active bundled cmd by polling each channel head.
    // Falls back to defaultCommand for any channel whose queue is empty
    // so the robot stays useful when an agent under-queues.
    function activeCommand(agent, dt) {
        var qs = queuesOf(agent);
        var def = defaultCommand();
        var move = advanceChannelHead(qs.move, dt);
        var aim  = advanceChannelHead(qs.aim,  dt);
        var fire = advanceChannelHead(qs.fire, dt);
        var cast = advanceChannelHead(qs.cast, dt);
        var cmd = {
            move: move || def.move,
            target: def.target,
            fireBasic: fire !== null ? !!(fire.enabled !== undefined ? fire.enabled : fire) : def.fireBasic,
            allowAbilities: cast || undefined,
        };
        if (aim) {
            if (aim.lookYaw !== undefined) cmd.lookYaw = aim.lookYaw;
            else cmd.target = aim;
        }
        return cmd;
    }

    function current(self) {
        // Peek without advancing — useful for debug/UI.
        var qs = queuesOf(self.agent);
        var def = defaultCommand();
        var moveP = qs.move[0] ? qs.move[0].partial : def.move;
        var aimP  = qs.aim[0]  ? qs.aim[0].partial  : null;
        var fireP = qs.fire[0] ? qs.fire[0].partial : def.fireBasic;
        var castP = qs.cast[0] ? qs.cast[0].partial : undefined;
        var cmd = {
            move: moveP,
            target: def.target,
            fireBasic: !!(fireP && fireP.enabled !== undefined ? fireP.enabled : fireP),
            allowAbilities: castP,
        };
        if (aimP) {
            if (aimP.lookYaw !== undefined) cmd.lookYaw = aimP.lookYaw;
            else cmd.target = aimP;
        }
        return cmd;
    }

    function queueLength(self) {
        // Legacy: report the longest of the four channels.
        var qs = queuesOf(self.agent);
        return Math.max(qs.move.length, qs.aim.length, qs.fire.length, qs.cast.length);
    }

    // Fallback when no command is queued. Matches scripted's default
    // engage behavior — nearest + LOS, fire, close to kite band, strafe.
    function defaultCommand() {
        return {
            target: { policy: "nearest", requireLOS: true },
            fireBasic: true,
            move: { mode: "advance", kiteBand: [0.45, 0.85], space: true },
        };
    }

    // ── Public entry point ───────────────────────────────────────────────
    //
    // Call once per think tick. `self` is the bound AgentBinding; dt is
    // game-seconds since last tick. Reads the active command from this
    // agent's queue (or defaultCommand if empty), handles aim + fire as
    // side-effects, then dispatches the single action (cast OR move OR
    // hold) that this tick commits to the engine's pending-action slot.
    function tick(self, dt) {
        var agent = self.agent;
        var u = agent.unit;
        if (!u.alive) { self.hold(0.5); return; }
        var mem = AI.getMem(u.id);

        var cmd = activeCommand(agent, dt);

        // Build a per-tick context from AI.shared so the robot doesn't need
        // the planner to wire up every field.
        var myTeam = u.teamId;
        var ctx = {
            simT: AI.shared.simT,
            world: AI.shared.world,
            nav: AI.shared.nav,
            obstacles: AI.shared.obstacles || [],
            enemies: AI.shared.teams[1 - myTeam] || [],
            teammates: AI.shared.teams[myTeam] || [],
            teamFocus: AI.shared.teamFocus[myTeam] || null,
            abilityCooldowns: (Arena.scenario && Arena.scenario.abilities)
                ? Arena.scenario.abilities.reduce(function (acc, ab) {
                    acc[ab.slot] = ab.cooldown; return acc;
                  }, {})
                : {},
        };

        // Decay ability cooldown mirror same way ai.js does.
        if (mem.abCd) {
            for (var k = 0; k < mem.abCd.length; k++) {
                if (mem.abCd[k] > 0) mem.abCd[k] -= dt;
            }
        }

        var target = pickTarget(agent, mem, cmd, ctx);
        mem.targetId = target ? target.unit.id : null;

        // Aim + basic-shot is a SIDE EFFECT — runs alongside whatever action
        // we ultimately queue below. This is how scripted gets to fire while
        // moving: spawnProjectile doesn't use the pending-Action slot.
        handleAim(agent, mem, target, cmd, ctx, dt);

        // Ability autocast consumes the tick if it fires.
        if (maybeCast(self, agent, mem, target, cmd, ctx)) return;

        handleMove(self, agent, mem, target, cmd, ctx);
    }

    return {
        tick: tick,
        push: push,
        replace: replace,
        clear: clear,
        current: current,
        queueLength: queueLength,
        defaultCommand: defaultCommand,
        pickTarget: pickTarget,  // exposed for planners that want to reuse
        // Per-channel API — preferred for agents that want independent
        // streams (move/aim/fire/cast). See queueOf for raw queue access.
        pushChannel: pushChannel,
        replaceChannel: replaceChannel,
        clearChannel: clearChannel,
        channelLength: channelLength,
        queuesOf: queuesOf,
        CHANNELS: CHANNELS,
        MAX_QUEUE: MAX_QUEUE,
    };
})();
