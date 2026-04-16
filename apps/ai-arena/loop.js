// loop.js — Fixed-step simulation tick + per-frame render pump. HUD panel
// updates are throttled at Config.*_HZ rather than firing every rAF frame
// (DOM mutations are expensive).
var Loop = {};
(function () {
    "use strict";

    function rosterName(id) {
        var R = Arena.ROSTER;
        for (var i = 0; i < R.length; i++) if (R[i].id === id) return R[i].name;
        return "?";
    }

    // One fixed-step tick of the sim: policies → movement → fire/ability →
    // world tick → damage event drain.
    Loop.simStep = function (state, dt) {
        var w = state.world;

        // Reset this tick's claimed-cover list so wounded ralliers fan out
        // across the frame rather than all converging on one cell.
        AI.resetClaimedCover();

        var teams = [[], []];
        var all = state.agents;
        for (var t = 0; t < all.length; t++) {
            teams[all[t].unit.teamId].push(all[t]);
        }
        var focusTarget = [null, null];
        focusTarget[0] = AI.chooseTeamFocus(teams[0], teams[1], Arena.OBSTACLES);
        focusTarget[1] = AI.chooseTeamFocus(teams[1], teams[0], Arena.OBSTACLES);
        state.teamFocus = focusTarget;

        for (var i = 0; i < all.length; i++) {
            var a = all[i];
            if (!a.unit.alive) continue;

            AI.decayThreat(AI.getMem(a.unit.id), dt);

            var myTeam = a.unit.teamId;
            var myMates = teams[myTeam];
            var myEnemies = teams[1 - myTeam];
            var myFocus = focusTarget[myTeam];

            // Remember last walkable anchor so we can revert if the
            // integrator drifts the agent onto a blocked cell.
            var preX = a.x, preZ = a.z;
            var preWalkable = state.nav.isWalkable(preX, preZ);

            var action;
            var myTarget = null;
            if (myTeam === 1 && state.blueAi === "mcts" && state.mcts[a.unit.id]) {
                var m = state.mcts[a.unit.id];
                var ca = AI.mctsStep(a, w, m.mcts, m.cache);
                state.lastMctsStats = m.mcts.lastStats;
                action = AI.applyMcts(a, w, ca, dt);
                AI.getMem(a.unit.id).intent = "MCTS";
            } else {
                myTarget = AI.pickTargetFor(a, myEnemies, myFocus, Arena.OBSTACLES);
                action = AI.scriptedTactical(
                    a, w, state.nav, myEnemies, myMates, myTarget,
                    Arena.OBSTACLES, state.elapsed);
                a.update(dt);
            }

            // Aim: scripted already called requestAim at decision points;
            // for MCTS (and as a safety net) seed desiredAim from the best
            // target. Snap yaw to the aim direction so strafers don't end
            // up oriented sideways to their target.
            var aimMem = AI.getMem(a.unit.id);
            var aimAt = action.fireAt ||
                (action.attackTargetId >= 0 ? state.byId[action.attackTargetId] : null) ||
                myTarget || myFocus;
            if (aimAt && aimAt.unit && aimAt.unit.alive) {
                var aimDx = aimAt.x - a.x, aimDz = aimAt.z - a.z;
                AI.requestAim(aimMem, state.elapsed, aimDx, aimDz);
                a.setYaw(Math.atan2(aimDx, -aimDz));
            }
            BotAim.tick(aimMem.aim, dt);

            if (action.fireAt) {
                AI.tryShoot(a, w, action.fireAt, Arena.OBSTACLES, aimMem, dt);
            } else if (action.attackTargetId >= 0) {
                w.resolveAttack(a, action.attackTargetId);
            }

            // Post-integrate collision: if steering drifted the agent onto
            // a blocked cell (corner clipping), snap back to the last
            // walkable anchor; clearTarget lets velocity decay before the
            // next attempt so we don't oscillate.
            if (!state.nav.isWalkable(a.x, a.z)) {
                var anchor = preWalkable
                    ? { x: preX, z: preZ }
                    : AI.findWalkableNear(state.nav, a.x, a.z, 3);
                if (anchor) {
                    a.setPosition(anchor.x, anchor.z);
                    a.clearTarget();
                }
            }

            if (action.useAbilityId >= 0) {
                var tid = action.abilityTargetId >= 0 ? action.abilityTargetId : a.unit.id;
                w.resolveAbility(a, action.useAbilityId, tid);
            }
        }

        w.tick(dt);

        // Drain damage events into the log + FX layer.
        var evs = w.events;
        for (var e = 0; e < evs.length; e++) {
            var ev = evs[e];
            var attacker = state.byId[ev.attackerId];
            var target = state.byId[ev.targetId];
            if (!target) continue;
            AI.recordDamage(ev.targetId, ev.attackerId, ev.amount, state.elapsed);
            var aName = attacker ? rosterName(attacker.unit.id) : "?";
            var tName = rosterName(target.unit.id);
            var cls = attacker && attacker.unit.teamId === 0 ? "log-red" : "log-blue";
            var killMark = ev.killed ? "  +" : "";
            state.pendingLog.push({
                text: aName + " -> " + tName + "  -" + Math.round(ev.amount) + killMark,
                cls: ev.killed ? "log-kill" : cls,
            });
            Render.addDamageNumber(target.x, target.z, ev.amount,
                ev.killed ? "#ffd24a" : "#ffffff");
            if (ev.killed) {
                Render.addExplosion(target.x, target.z, 1.2);
            }
        }
        w.clearEvents();

        state.snapshotAccum += dt;
        if (state.snapshotAccum >= Config.SNAPSHOT_INTERVAL) {
            state.snapshotAccum = 0;
            state.snapshots.push({ t: state.elapsed, snap: w.snapshot() });
            while (state.snapshots.length > Config.SNAPSHOT_KEEP) state.snapshots.shift();
        }

        if (state.recording && state.recorder) {
            state.recorder.recordFrame(state.simSteps, state.elapsed, w);
        }

        state.simSteps++;
        state.elapsed += dt;
    };

    // Per-rAF render + throttled HUD updates. Called once per frame.
    Loop.frame = function (state, ctx, canvas, dt) {
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        Render.drawArena(ctx);
        Render.drawProjectiles(ctx, state.world.projectiles);
        Render.drawAgents(ctx, state.agents, state.focusId);
        Render.drawFx(ctx, dt);

        state.rosterAccum += dt;
        if (state.rosterAccum >= Config.ROSTER_HZ) {
            state.rosterAccum = 0;
            UI.updateRoster(state.agents);
            if (state.pendingLog.length) {
                for (var pl = 0; pl < state.pendingLog.length; pl++) {
                    UI.log(state.pendingLog[pl].text, state.pendingLog[pl].cls);
                }
                state.pendingLog.length = 0;
            }
        }

        state.obsAccum += dt;
        if (state.obsAccum >= Config.OBS_HZ) {
            state.obsAccum = 0;
            var focus = state.byId[state.focusId];
            if (focus) {
                try {
                    var obs = bro.ai.game.buildObservation(focus, state.world);
                    UI.drawObservation(obs);
                    var mask = bro.ai.game.buildActionMask(focus, state.world);
                    UI.drawActionMask(mask.mask);
                } catch (e) { /* observation may throw if agent dead */ }
            }
        }

        state.rewardAccum += dt;
        if (state.rewardAccum >= Config.REWARD_HZ) {
            state.rewardAccum = 0;
            var redD = 0, blueD = 0;
            for (var ri = 0; ri < state.agents.length; ri++) {
                var ra = state.agents[ri];
                var tr = state.rewards[ra.unit.id];
                if (!tr) continue;
                var d = tr.consume(ra, state.world);
                var r = d.damageDealt - d.damageTaken + d.kills * 20 - d.deaths * 20;
                if (ra.unit.teamId === 0) redD += r; else blueD += r;
            }
            UI.pushReward(redD, blueD);
            UI.drawReward();
        }

        state.statusAccum += dt;
        if (state.statusAccum >= Config.STATUS_HZ) {
            state.statusAccum = 0;
            UI.updateMctsStats(state.lastMctsStats, state.blueAi === "mcts");
            if (state.paused) UI.setStatus("paused");
            else if (state.recording) UI.setStatus("recording  " + state.recorder.frameCount + " frames");
            else UI.setStatus("running  t=" + state.elapsed.toFixed(1) + "s  steps=" + state.simSteps);
        }
    };
})();
