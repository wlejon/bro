// main.js — Entry point: build world, run game loop, wire UI, record/replay.
(function () {
    "use strict";

    var canvas = document.getElementById("arena");
    var ctx = canvas.getContext("2d");

    var state = null;

    function rebuild() {
        var built = Arena.build();
        var rewardTrackers = {};
        for (var i = 0; i < built.agents.length; i++) {
            var a = built.agents[i];
            rewardTrackers[a.unit.id] = bro.ai.game.createRewardTracker(a, built.world);
        }

        // One MCTS instance per Blue-team agent, with its own action cache.
        var mctsByAgent = {};
        for (var j = 0; j < built.agents.length; j++) {
            var ag = built.agents[j];
            if (ag.unit.teamId === 1) {
                mctsByAgent[ag.unit.id] = {
                    mcts: AI.createMcts(),
                    cache: { action: null, ttl: 0 },
                };
            }
        }

        state = {
            nav: built.nav,
            world: built.world,
            agents: built.agents,
            byId: built.byId,
            rewards: rewardTrackers,
            mcts: mctsByAgent,
            lastEventIdx: 0,
            snapshots: [],             // ring buffer of { t, snap }
            snapshotAccum: 0,
            blueAi: document.getElementById("sel-ai").value,
            paused: false,
            focusId: +document.getElementById("sel-focus").value,
            // Panel update cadences (separate from sim — DOM mutations are expensive)
            obsAccum: 0,
            rewardAccum: 0,
            rosterAccum: 0,
            statusAccum: 0,
            simAccum: 0,
            pendingLog: [],
            // Simulation wrapper for demo purposes (we don't actually drive via it —
            // we call policies manually so that abilities can be invoked. But we still
            // expose Simulation.createSimulation to document integration).
            sim: bro.ai.game.createSimulation(built.world),
            simSteps: 0,
            elapsed: 0,
            // Recording / replay
            recorder: null,
            recording: false,
            replayReader: null,
            replayFrame: 0,
            replayPlaying: false,
            replayElapsed: 0,
            lastMctsStats: null,
        };

        UI.rewardHistory = { red: [], blue: [] };
        UI.log("arena built — " + built.agents.length + " agents", "");
        UI.setStatus("running");
    }

    function simStep(dt) {
        var w = state.world;

        // Reset the per-tick list of claimed cover / rally points so
        // wounded ralliers fan out across this frame's search rather
        // than all converging on the same cell.
        AI.resetClaimedCover();

        // Partition agents by team so both scripted and MCTS policies
        // can reason about teammates and enemies (focus-fire, flee, etc).
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

            // Remember last walkable anchor so we can revert if the engine's
            // un-avoiding integrator drifts the agent onto a blocked cell.
            var preX = a.x, preZ = a.z;
            var preWalkable = state.nav.isWalkable(preX, preZ);

            var action;
            if (myTeam === 1 && state.blueAi === "mcts" && state.mcts[a.unit.id]) {
                var m = state.mcts[a.unit.id];
                var ca = AI.mctsStep(a, w, m.mcts, m.cache);
                state.lastMctsStats = m.mcts.lastStats;
                action = AI.applyMcts(a, w, ca, dt);
                AI.getMem(a.unit.id).intent = "MCTS";
                // Movement already applied inside applyMcts; do not call agent.update(dt).
            } else {
                var myTarget = AI.pickTargetFor(a, myEnemies, myFocus, Arena.OBSTACLES);
                action = AI.scriptedTactical(
                    a, w, state.nav, myEnemies, myMates, myTarget,
                    Arena.OBSTACLES, state.elapsed);
                a.update(dt);
            }

            // Ranged fire — spawns a projectile when LOS + cooldown allow.
            if (action.fireAt) {
                AI.tryShoot(a, w, action.fireAt, Arena.OBSTACLES, AI.getMem(a.unit.id), dt);
            } else if (action.attackTargetId >= 0) {
                // (legacy: used by MCTS `applyMcts` when it returns an attackSlot)
                w.resolveAttack(a, action.attackTargetId);
            }

            // Post-integrate collision: if steering drifted the agent onto a
            // blocked cell (corner clipping), snap it back. clearTarget lets
            // velocity decay before the next attempt so we don't oscillate.
            if (!state.nav.isWalkable(a.x, a.z)) {
                var anchor = preWalkable
                    ? { x: preX, z: preZ }
                    : AI.findWalkableNear(state.nav, a.x, a.z, 3);
                if (anchor) {
                    a.setPosition(anchor.x, anchor.z);
                    a.clearTarget();
                }
            }
            // Ability resolution
            if (action.useAbilityId >= 0) {
                var tid = action.abilityTargetId >= 0 ? action.abilityTargetId : a.unit.id;
                var ok = w.resolveAbility(a, action.useAbilityId, tid);
                if (ok && action.useAbilityId === Arena.AB_GRENADE) {
                    // Pre-queue explosion ring — actual splash happens when projectile lifetime ends.
                }
            }
            // (mana regen + cooldowns handled by world.tick below)
        }

        w.tick(dt);

        // Drain damage events into the log and FX layer.
        var evs = w.events;
        for (var e = 0; e < evs.length; e++) {
            var ev = evs[e];
            var attacker = state.byId[ev.attackerId];
            var target = state.byId[ev.targetId];
            if (!target) continue;
            // Feed the threat tracker so cover/retreat logic knows who's
            // actually being shot at and by whom.
            AI.recordDamage(ev.targetId, ev.attackerId, ev.amount, state.elapsed);
            var aName = attacker ? attackerName(attacker) : "?";
            var tName = targetName(target);
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

        // Snapshot ring buffer — store every 1.0 s, keep last 5.
        state.snapshotAccum += dt;
        if (state.snapshotAccum >= 1.0) {
            state.snapshotAccum = 0;
            state.snapshots.push({ t: state.elapsed, snap: w.snapshot() });
            while (state.snapshots.length > 5) state.snapshots.shift();
        }

        // Recorder frame
        if (state.recording && state.recorder) {
            state.recorder.recordFrame(state.simSteps, state.elapsed, w);
        }

        state.simSteps++;
        state.elapsed += dt;
    }

    function attackerName(a) {
        for (var i = 0; i < Arena.ROSTER.length; i++)
            if (Arena.ROSTER[i].id === a.unit.id) return Arena.ROSTER[i].name;
        return "?";
    }
    function targetName(a) { return attackerName(a); }

    // ── Main loop ─────────────────────────────────────────────────────────
    var lastT = performance.now();
    function frame(now) {
        requestAnimationFrame(frame);
        var dt = (now - lastT) / 1000;
        lastT = now;
        if (dt > 0.1) dt = 0.1;

        if (!state) return;

        // Refresh focus from selector (may change mid-run)
        state.focusId = +document.getElementById("sel-focus").value;
        state.blueAi = document.getElementById("sel-ai").value;

        // Replay mode draws frames from a loaded .bgar file.
        if (state.replayPlaying && state.replayReader) {
            ctx.clearRect(0, 0, canvas.width, canvas.height);
            Render.drawArena(ctx);
            var fc = state.replayReader.frameCount;
            var f = state.replayReader.frame(state.replayFrame);
            Render.drawReplayFrame(ctx, f, state.focusId);
            state.replayElapsed += dt;
            if (state.replayElapsed > 0.033) {
                state.replayElapsed = 0;
                state.replayFrame = (state.replayFrame + 1) % Math.max(1, fc);
            }
            UI.setStatus("replay — frame " + state.replayFrame + "/" + fc);
            return;
        }

        if (!state.paused) {
            // Fixed-step simulation with an accumulator that persists
            // across frames, so sub-step leftovers aren't dropped.
            var stepDt = 1 / 60;
            var maxSteps = 8;
            state.simAccum += dt;
            while (state.simAccum >= stepDt && maxSteps-- > 0) {
                simStep(stepDt);
                state.simAccum -= stepDt;
            }
            // Clamp runaway accum when the frame stalls for a long time.
            if (state.simAccum > 0.25) state.simAccum = 0.25;
        }

        // Render
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        Render.drawArena(ctx);
        Render.drawProjectiles(ctx, state.world.projectiles);
        Render.drawAgents(ctx, state.agents, state.focusId);
        Render.drawFx(ctx, dt);

        // UI panels — throttled, DOM mutations are expensive.
        state.rosterAccum += dt;
        if (state.rosterAccum >= 0.2) {
            state.rosterAccum = 0;
            UI.updateRoster(state.agents);
            // Flush any batched log entries at the same cadence.
            if (state.pendingLog.length) {
                for (var pl = 0; pl < state.pendingLog.length; pl++) {
                    UI.log(state.pendingLog[pl].text, state.pendingLog[pl].cls);
                }
                state.pendingLog.length = 0;
            }
        }

        state.obsAccum += dt;
        if (state.obsAccum >= 0.1) {
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
        if (state.rewardAccum >= 0.25) {
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
        if (state.statusAccum >= 0.25) {
            state.statusAccum = 0;
            UI.updateMctsStats(state.lastMctsStats, state.blueAi === "mcts");
            if (state.paused) UI.setStatus("paused");
            else if (state.recording) UI.setStatus("recording  " + state.recorder.frameCount + " frames");
            else UI.setStatus("running  t=" + state.elapsed.toFixed(1) + "s  steps=" + state.simSteps);
        }
    }

    // ── Controls ─────────────────────────────────────────────────────────
    function bindControls() {
        document.getElementById("btn-pause").addEventListener("click", function () {
            state.paused = !state.paused;
            this.textContent = state.paused ? "Play" : "Pause";
        });

        document.getElementById("btn-rewind").addEventListener("click", function () {
            if (!state.snapshots.length) { UI.log("rewind: no snapshot yet"); return; }
            // Pick snapshot that is ~2s old, else the oldest.
            var target = state.snapshots[0];
            for (var i = 0; i < state.snapshots.length; i++) {
                if (state.elapsed - state.snapshots[i].t >= 1.5) target = state.snapshots[i];
            }
            state.world.restore(target.snap);
            UI.log("rewound to t=" + target.t.toFixed(1) + "s", "log-kill");
        });

        document.getElementById("btn-record").addEventListener("click", function () {
            if (!state.recording) {
                state.recorder = bro.ai.game.createRecorder();
                var path = "apps/ai-arena/replays/arena-" + Date.now() + ".bgar";
                var ok = state.recorder.open(path, 1, Date.now(), 1 / 60);
                if (!ok) { UI.log("recorder open failed: " + path); return; }
                state.recorder.writeRoster(state.world);
                state.recording = true;
                this.textContent = "Stop Rec";
                this.classList.add("active");
                state._recordingPath = path;
                UI.log("recording → " + path, "log-kill");
            } else {
                state.recorder.close();
                state.recording = false;
                this.textContent = "Record";
                this.classList.remove("active");
                UI.log("recording stopped (" + state.recorder.frameCount + " frames)", "log-kill");
            }
        });

        document.getElementById("btn-play").addEventListener("click", function () {
            if (state.replayPlaying) {
                state.replayPlaying = false;
                state.replayReader = null;
                this.textContent = "Play";
                this.classList.remove("active");
                UI.log("replay stopped");
                return;
            }
            var path = state._recordingPath;
            if (!path) { UI.log("no replay to play — record one first"); return; }
            var rr = bro.ai.game.createReplayReader();
            var ok = rr.open(path);
            if (!ok) { UI.log("replay open failed: " + rr.errorMessage); return; }
            state.replayReader = rr;
            state.replayFrame = 0;
            state.replayPlaying = true;
            this.textContent = "Stop Play";
            this.classList.add("active");
            UI.log("playing replay — " + rr.frameCount + " frames", "log-kill");
        });

        document.getElementById("btn-reset").addEventListener("click", function () {
            if (state && state.recording && state.recorder) state.recorder.close();
            rebuild();
            document.getElementById("btn-pause").textContent = "Pause";
        });
    }

    // ── Init ─────────────────────────────────────────────────────────────
    UI.init();
    rebuild();
    bindControls();
    // Debug hook — exposes `getState` globally so headless scripts can inspect.
    window.getState = function () { return state; };
    requestAnimationFrame(frame);

    console.log("ai-arena started");
})();
