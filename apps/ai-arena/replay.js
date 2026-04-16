// replay.js — Record + playback state machine. Owned by the Record/Play
// buttons in controls.js; main.js consults state.replayPlaying each frame
// and delegates to Replay.drawFrame when true.
var Replay = {};
(function () {
    "use strict";

    // Replay playback paused during the 3D scene migration — the old 2D
    // renderer is gone and the scene-backed version lands in Phase 5.
    // Recording still works; only playback is disabled.
    Replay.drawFrame = function (state, canvas, dt) {
        UI.setStatus("replay playback disabled during 3D refactor");
        state.replayPlaying = false;
    };

    Replay.toggleRecord = function (state, btn) {
        if (!state.recording) {
            state.recorder = bro.ai.game.createRecorder();
            var path = Config.REPLAY_DIR + "arena-" + Date.now() + ".bgar";
            var ok = state.recorder.open(path, 1, Date.now(), Config.SIM_STEP);
            if (!ok) { UI.log("recorder open failed: " + path); return; }
            state.recorder.writeRoster(state.world);
            state.recording = true;
            btn.textContent = "Stop Rec";
            btn.classList.add("active");
            state._recordingPath = path;
            UI.log("recording -> " + path, "log-kill");
        } else {
            state.recorder.close();
            state.recording = false;
            btn.textContent = "Record";
            btn.classList.remove("active");
            UI.log("recording stopped (" + state.recorder.frameCount + " frames)", "log-kill");
        }
    };

    Replay.togglePlay = function (state, btn) {
        if (state.replayPlaying) {
            state.replayPlaying = false;
            state.replayReader = null;
            btn.textContent = "Play";
            btn.classList.remove("active");
            UI.log("replay stopped");
            return;
        }
        var path = state._recordingPath;
        if (!path) { UI.log("no replay to play - record one first"); return; }
        var rr = bro.ai.game.createReplayReader();
        var ok = rr.open(path);
        if (!ok) { UI.log("replay open failed: " + rr.errorMessage); return; }
        state.replayReader = rr;
        state.replayFrame = 0;
        state.replayPlaying = true;
        btn.textContent = "Stop Play";
        btn.classList.add("active");
        UI.log("playing replay - " + rr.frameCount + " frames", "log-kill");
    };

    Replay.rewind = function (state) {
        if (!state.snapshots.length) { UI.log("rewind: no snapshot yet"); return; }
        // Pick a snapshot that is at least REWIND_SECONDS old; else oldest.
        var target = state.snapshots[0];
        for (var i = 0; i < state.snapshots.length; i++) {
            if (state.elapsed - state.snapshots[i].t >= Config.REWIND_SECONDS) {
                target = state.snapshots[i];
            }
        }
        state.world.restore(target.snap);
        UI.log("rewound to t=" + target.t.toFixed(1) + "s", "log-kill");
    };
})();
