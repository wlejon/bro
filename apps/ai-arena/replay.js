// replay.js — Record + playback state machine. Owned by the Record/Play
// buttons in controls.js; main.js consults state.replayPlaying each frame
// and delegates to Replay.drawFrame when true.
var Replay = {};
(function () {
    "use strict";

    Replay.drawFrame = function (state, ctx, canvas, dt) {
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        Render.drawArena(ctx);
        var fc = state.replayReader.frameCount;
        var f = state.replayReader.frame(state.replayFrame);
        Render.drawReplayFrame(ctx, f, state.focusId);
        state.replayElapsed += dt;
        if (state.replayElapsed > Config.REPLAY_FRAME_DT) {
            state.replayElapsed = 0;
            state.replayFrame = (state.replayFrame + 1) % Math.max(1, fc);
        }
        UI.setStatus("replay - frame " + state.replayFrame + "/" + fc);
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
