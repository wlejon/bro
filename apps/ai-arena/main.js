// main.js — Bootstrap: load the default scenario, wire controls, run the
// rAF loop. All gameplay tick logic lives in loop.js; buttons in
// controls.js; record/play in replay.js; world build in arena.js.
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
        var built = Arena.build(App.scenario);
        Scene3D.build(App.scenario);

        var rewardTrackers = {};
        for (var i = 0; i < built.agents.length; i++) {
            var a = built.agents[i];
            rewardTrackers[a.unit.id] = bro.ai.game.createRewardTracker(a, built.world);
        }

        // One MCTS instance per Blue-team agent, with its own action cache.
        // Stagger the initial TTLs so searches (12 ms budget each) don't
        // all land on the same frame — otherwise N agents × 12 ms spikes
        // every 15 frames, starving the render loop.
        var mctsByAgent = {};
        var mctsCount = 0;
        for (var j = 0; j < built.agents.length; j++) {
            var ag = built.agents[j];
            if (ag.unit.teamId === 1) {
                mctsByAgent[ag.unit.id] = {
                    mcts: AI.createMcts(),
                    cache: { action: null, ttl: mctsCount % 15 },
                };
                mctsCount++;
            }
        }

        App.state = {
            nav: built.nav,
            world: built.world,
            agents: built.agents,
            byId: built.byId,
            rewards: rewardTrackers,
            mcts: mctsByAgent,
            snapshots: [],
            snapshotAccum: 0,
            blueAi: "scripted",
            paused: false,
            focusId: -1,
            obsAccum: 0,
            rewardAccum: 0,
            rosterAccum: 0,
            statusAccum: 0,
            simAccum: 0,
            pendingLog: [],
            // Simulation wrapper — documented integration point; we drive
            // policies manually so ability casts flow through our loop.
            sim: bro.ai.game.createSimulation(built.world),
            simSteps: 0,
            elapsed: 0,
            recorder: null,
            recording: false,
            replayReader: null,
            replayFrame: 0,
            replayPlaying: false,
            replayElapsed: 0,
            lastMctsStats: null,
            teamFocus: [null, null],
        };

        AI.memory = {};
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
            var stepDt = Config.SIM_STEP;
            var maxSteps = Config.MAX_STEPS_PER_FRAME;
            state.simAccum += dt;
            while (state.simAccum >= stepDt && maxSteps-- > 0) {
                Loop.simStep(state, stepDt);
                state.simAccum -= stepDt;
            }
            if (state.simAccum > Config.MAX_SIM_ACCUM) {
                state.simAccum = Config.MAX_SIM_ACCUM;
            }
        }

        Loop.frame(state, App.canvas, dt);
    }

    App.canvas = document.getElementById("arena");
    App.scenario = Scenarios.ALL[0];

    UI.init();
    Scene3D.init(App.canvas);
    App.rebuild();
    Controls.bind(App.rebuild);

    // Debug hook — exposes state globally so headless scripts can inspect.
    window.getState = function () { return App.state; };

    lastT = performance.now();
    requestAnimationFrame(frame);

    console.log("ai-arena started");
})();
