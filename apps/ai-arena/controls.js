// controls.js — Button handlers + event-driven selector state. Controls
// read App.state each time a handler fires (not at bind time), so state
// object swaps from App.rebuild() propagate without rebinding.
var Controls = {};
(function () {
    "use strict";

    Controls.bind = function (onReset) {
        var btnPause  = document.getElementById("btn-pause");
        var btnRewind = document.getElementById("btn-rewind");
        var btnRecord = document.getElementById("btn-record");
        var btnPlay   = document.getElementById("btn-play");
        var btnReset  = document.getElementById("btn-reset");
        var selAi     = document.getElementById("sel-ai");
        var selFocus  = document.getElementById("sel-focus");

        btnPause.addEventListener("click", function () {
            var s = App.state;
            s.paused = !s.paused;
            btnPause.textContent = s.paused ? "Resume" : "Pause";
        });
        btnRewind.addEventListener("click", function () { Replay.rewind(App.state); });
        btnRecord.addEventListener("click", function () { Replay.toggleRecord(App.state, btnRecord); });
        btnPlay.addEventListener("click",   function () { Replay.togglePlay(App.state, btnPlay); });
        btnReset.addEventListener("click",  function () {
            var s = App.state;
            if (s && s.recording && s.recorder) s.recorder.close();
            onReset();
            btnPause.textContent = "Pause";
            btnPlay.textContent = "Play";
            btnPlay.classList.remove("active");
            btnRecord.textContent = "Record";
            btnRecord.classList.remove("active");
        });

        // Event-driven: update state once on change rather than polling
        // the DOM every rAF frame.
        selAi.addEventListener("change", function () { App.state.blueAi = selAi.value; });
        selFocus.addEventListener("change", function () { App.state.focusId = +selFocus.value; });
    };

    // Seed state from the current selector values. Called after rebuild
    // since the focus dropdown was just regenerated for the new roster.
    Controls.syncFromDom = function (state) {
        state.blueAi = document.getElementById("sel-ai").value;
        state.focusId = +document.getElementById("sel-focus").value;
    };
})();
