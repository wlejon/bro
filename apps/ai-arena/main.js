// main.js — Bootstrap. Scene graph drives the sim via attachAIWorld, each
// capsule node owns an agent binding that dispatches to the registered
// agent (Agents.thinkFor) per tick; the agent then issues a plan into
// Bot's queue and calls Bot.tick for execution. The rAF loop is thin:
// refresh shared AI state each frame, then pump HUD updates and drain
// damage events post-tick.
//
// The Red AI / Blue AI selectors pick which registered agent runs for
// each team's units — see agents/registry.js.
var App = {};
(function () {
    "use strict";

    App.state = null;
    App.canvas = null;
    App.scenario = null;

    App.setScenario = function (scenario) {
        App.scenario = scenario;
        App.rebuild();
    };

    App.rebuild = function () {
        if (App.state && App.state.world) {
            Scene3D.scene.detachAIWorld();
            for (var di = 0; di < App.state.agents.length; di++) {
                var dn = Scene3D.units[App.state.agents[di].unit.id];
                if (dn) { try { dn.detachAgent(); } catch (e) {} }
            }
        }

        var built = Arena.build(App.scenario);
        Scene3D.build(App.scenario);

        var rewardTrackers = {};
        for (var i = 0; i < built.agents.length; i++) {
            var a = built.agents[i];
            rewardTrackers[a.unit.id] = bro.ai.game.createRewardTracker(a, built.world);
        }

        App.state = {
            nav: built.nav,
            world: built.world,
            agents: built.agents,
            byId: built.byId,
            rewards: rewardTrackers,
            snapshots: [],
            snapshotAccum: 0,
            redAi: "scripted",
            blueAi: "scripted",
            paused: false,
            focusId: -1,
            obsAccum: 0,
            rewardAccum: 0,
            rosterAccum: 0,
            statusAccum: 0,
            pendingLog: [],
            simSteps: 0,
            elapsed: 0,
            recorder: null,
            recording: false,
            replayReader: null,
            replayFrame: 0,
            replayPlaying: false,
            replayElapsed: 0,
            agentStats: null,
        };

        AI.memory = {};

        // Ensure shared state is populated before the first think() fires —
        // attachAIWorld/attachAgent immediately schedule a tick.
        AI.updateShared(App.state);

        Scene3D.scene.attachAIWorld(built.world, {
            stepHz: 60, maxStepsPerFrame: Config.MAX_STEPS_PER_FRAME,
        });

        // Bind each unit capsule to its agent with the scripted think().
        var CAPS = ["move_to", "cast_ability", "flee", "hold", "aimed_shot"];
        for (var j = 0; j < built.agents.length; j++) {
            var ag = built.agents[j];
            var node = Scene3D.units[ag.unit.id];
            if (!node) continue;
            node.attachAgent(built.world, ag, {
                capabilities: CAPS,
                thinkHz: 30,
                faceMovement: true,
                yOffset: Scene3D.UNIT_Y,
                think: Agents.thinkFor,
            });
        }

        Render.clearFx();
        UI.rebuildRoster(Arena.ROSTER);
        Controls.syncFromDom(App.state);
        UI.rewardHistory = { red: [], blue: [] };
        UI.log("arena built - " + built.agents.length + " agents (" +
               App.scenario.name + ")", "");
        UI.setStatus("running");
    };

    var lastT = 0;
    function frame(now) {
        requestAnimationFrame(frame);
        var dt = (now - lastT) / 1000;
        lastT = now;
        if (dt > 0.1) dt = 0.1;

        var state = App.state;
        if (!state) return;

        if (state.replayPlaying && state.replayReader) {
            Replay.drawFrame(state, App.canvas, dt);
            return;
        }

        if (!state.paused) {
            AI.updateShared(state);
            // Team-level planners (portfolio search, influence maps) run
            // before per-agent think fires inside the scene AI tick.
            Agents.tickTeams(state, dt);
            state.elapsed += dt;
            state.simSteps = Math.round(state.elapsed / Config.SIM_STEP);
        }

        Loop.frame(state, App.canvas, dt);
    }

    App.canvas = document.getElementById("arena");
    App.scenario = Scenarios.ALL[0];

    UI.init();
    Scene3D.init(App.canvas);
    Controls.populateSelectors("scripted", "scripted");
    App.rebuild();
    Controls.bind(App.rebuild);

    // Debug hook — exposes state globally so headless scripts can inspect.
    window.getState = function () { return App.state; };

    lastT = performance.now();
    requestAnimationFrame(frame);

    console.log("ai-arena started");
})();
