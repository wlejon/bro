// controls.js — Button handlers + event-driven selector state. Controls
// read App.state each time a handler fires (not at bind time), so state
// object swaps from App.rebuild() propagate without rebinding.
var Controls = {};
(function () {
    "use strict";

    // Populate the Red/Blue AI selectors from the Agents registry.
    // Called once at startup; selector options persist across rebuilds.
    Controls.populateSelectors = function (defaultRed, defaultBlue) {
        var selRed  = document.getElementById("sel-red-ai");
        var selBlue = document.getElementById("sel-blue-ai");
        var defs = Agents.all();
        [selRed, selBlue].forEach(function (sel) {
            while (sel.firstChild) sel.removeChild(sel.firstChild);
            for (var i = 0; i < defs.length; i++) {
                var opt = document.createElement("option");
                opt.value = defs[i].id;
                opt.textContent = defs[i].label;
                sel.appendChild(opt);
            }
        });
        if (defaultRed)  selRed.value  = defaultRed;
        if (defaultBlue) selBlue.value = defaultBlue;
    };

    Controls.bind = function (onReset) {
        var btnPause  = document.getElementById("btn-pause");
        var btnRewind = document.getElementById("btn-rewind");
        var btnRecord = document.getElementById("btn-record");
        var btnPlay   = document.getElementById("btn-play");
        var btnReset  = document.getElementById("btn-reset");
        var selRed    = document.getElementById("sel-red-ai");
        var selBlue   = document.getElementById("sel-blue-ai");
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

        selRed.addEventListener("change",  function () { App.state.redAi  = selRed.value;  });
        selBlue.addEventListener("change", function () { App.state.blueAi = selBlue.value; });
        selFocus.addEventListener("change", function () { App.state.focusId = +selFocus.value; });
    };

    // Seed state from the current selector values. Called after rebuild
    // since the focus dropdown was just regenerated for the new roster.
    Controls.syncFromDom = function (state) {
        var r = document.getElementById("sel-red-ai");
        var b = document.getElementById("sel-blue-ai");
        var f = document.getElementById("sel-focus");
        if (r && r.value) state.redAi  = r.value;
        if (b && b.value) state.blueAi = b.value;
        if (f && f.value) state.focusId = +f.value;
    };
})();
