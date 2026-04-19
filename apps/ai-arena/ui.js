// ui.js — HUD panel updates: roster, observation bars, action mask, reward
// chart, log. Element creation is split between init() (static panels) and
// rebuildRoster() (scenario-dependent roster rows + focus options).
var UI = {};
(function () {
    "use strict";

    UI.rosterEl = null;
    UI.logEl = null;
    UI.maskRowEl = null;
    UI.agentStatsEl = null;
    UI.statusEl = null;
    UI.rewardNumsEl = null;
    UI.obsCanvas = null;
    UI.obsCtx = null;
    UI.rewardCanvas = null;
    UI.rewardCtx = null;
    UI.rosterRows = [];
    UI.MAX_HIST = 200;
    UI.rewardHistory = { red: [], blue: [] };

    // Number of action-mask cells (contract with bro.ai.game.buildActionMask).
    var MASK_CELLS = 13;

    UI.init = function () {
        UI.rosterEl = document.getElementById("roster");
        UI.logEl = document.getElementById("log");
        UI.maskRowEl = document.getElementById("mask-row");
        UI.agentStatsEl = document.getElementById("agent-stats");
        UI.statusEl = document.getElementById("status");
        UI.rewardNumsEl = document.getElementById("reward-nums");
        UI.obsCanvas = document.getElementById("obs-canvas");
        UI.obsCtx = UI.obsCanvas.getContext("2d");
        UI.rewardCanvas = document.getElementById("reward-canvas");
        UI.rewardCtx = UI.rewardCanvas.getContext("2d");

        UI.MAX_HIST = Config.REWARD_HISTORY;

        while (UI.maskRowEl.firstChild) UI.maskRowEl.removeChild(UI.maskRowEl.firstChild);
        for (var i = 0; i < MASK_CELLS; i++) {
            var d = document.createElement("div");
            d.className = "mask-cell";
            UI.maskRowEl.appendChild(d);
        }
    };

    // Build roster rows and focus dropdown from the scenario roster. Called
    // after every Arena.build so scenarios with different rosters update.
    UI.rebuildRoster = function (roster) {
        while (UI.rosterEl.firstChild) UI.rosterEl.removeChild(UI.rosterEl.firstChild);
        UI.rosterRows = [];
        for (var j = 0; j < roster.length; j++) {
            var row = document.createElement("div");
            row.className = "roster-row";
            UI.rosterEl.appendChild(row);
            UI.rosterRows.push(row);
        }

        var focusSel = document.getElementById("sel-focus");
        while (focusSel.firstChild) focusSel.removeChild(focusSel.firstChild);
        for (var k = 0; k < roster.length; k++) {
            var r = roster[k];
            var opt = document.createElement("option");
            opt.value = String(r.id);
            opt.textContent = (r.teamId === 0 ? "[R] " : "[B] ") + r.name;
            if (r.teamId === 1 && k === roster.length / 2) opt.selected = true;
            focusSel.appendChild(opt);
        }
    };

    UI.updateRoster = function (agents) {
        for (var i = 0; i < agents.length; i++) {
            var a = agents[i];
            var r = Arena.ROSTER[i];
            var row = UI.rosterRows[i];
            if (!row) continue;
            var prefix = a.unit.teamId === 0 ? "[R] " : "[B] ";
            var hp = Math.max(0, Math.round(a.unit.hp));
            var mp = Math.round(a.unit.mana);
            row.textContent = prefix + r.name + "  HP " + hp + "  MP " + mp +
                (a.unit.alive ? "" : "  (DEAD)");
            row.className = "roster-row" + (a.unit.alive ? "" : " roster-dead");
        }
    };

    UI.log = function (text, cls) {
        var now = new Date();
        var ts = String(now.getMinutes()).padStart(2, "0") + ":" +
                 String(now.getSeconds()).padStart(2, "0");
        var line = document.createElement("div");
        line.className = cls || "";
        line.textContent = ts + "  " + text;
        UI.logEl.appendChild(line);
        while (UI.logEl.childNodes.length > Config.LOG_LINES) {
            UI.logEl.removeChild(UI.logEl.firstChild);
        }
        UI.logEl.scrollTop = UI.logEl.scrollHeight;
    };

    UI.drawObservation = function (obs) {
        var c = UI.obsCanvas, ctx = UI.obsCtx;
        ctx.fillStyle = "#0d1014";
        ctx.fillRect(0, 0, c.width, c.height);
        if (!obs || !obs.length) return;
        var n = obs.length;
        var bw = c.width / n;
        for (var i = 0; i < n; i++) {
            var v = obs[i];
            if (v > 1) v = 1; if (v < -1) v = -1;
            var mid = c.height * 0.5;
            var bar = Math.abs(v) * (c.height * 0.48);
            ctx.fillStyle = v >= 0 ? "#4a8ad4" : "#d46a4a";
            if (v >= 0) ctx.fillRect(i * bw, mid - bar, bw - 0.5, bar);
            else ctx.fillRect(i * bw, mid, bw - 0.5, bar);
        }
        ctx.strokeStyle = "#2a2f36";
        ctx.beginPath();
        ctx.moveTo(0, c.height * 0.5);
        ctx.lineTo(c.width, c.height * 0.5);
        ctx.stroke();
    };

    UI.drawActionMask = function (mask) {
        var cells = UI.maskRowEl.children;
        if (!mask || !mask.length) return;
        var n = Math.min(cells.length, mask.length);
        for (var i = 0; i < n; i++) {
            var v = mask[i];
            cells[i].className = "mask-cell " + (v > 0.5 ? "mask-on" : "mask-off");
        }
    };

    UI.pushReward = function (redDelta, blueDelta) {
        var hr = UI.rewardHistory;
        var lastRed = hr.red.length ? hr.red[hr.red.length - 1] : 0;
        var lastBlue = hr.blue.length ? hr.blue[hr.blue.length - 1] : 0;
        hr.red.push(lastRed + redDelta);
        hr.blue.push(lastBlue + blueDelta);
        while (hr.red.length > UI.MAX_HIST) hr.red.shift();
        while (hr.blue.length > UI.MAX_HIST) hr.blue.shift();
    };

    UI.drawReward = function () {
        var c = UI.rewardCanvas, ctx = UI.rewardCtx;
        ctx.fillStyle = "#0d1014";
        ctx.fillRect(0, 0, c.width, c.height);
        var hr = UI.rewardHistory;
        if (!hr.red.length) return;

        var maxAbs = 1;
        for (var i = 0; i < hr.red.length; i++) {
            if (Math.abs(hr.red[i]) > maxAbs) maxAbs = Math.abs(hr.red[i]);
            if (Math.abs(hr.blue[i]) > maxAbs) maxAbs = Math.abs(hr.blue[i]);
        }
        function drawLine(arr, color) {
            ctx.strokeStyle = color;
            ctx.lineWidth = 1.5;
            ctx.beginPath();
            for (var j = 0; j < arr.length; j++) {
                var x = (j / (UI.MAX_HIST - 1)) * c.width;
                var y = c.height * 0.5 - (arr[j] / maxAbs) * (c.height * 0.45);
                if (j === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
            }
            ctx.stroke();
        }
        ctx.strokeStyle = "#2a2f36";
        ctx.beginPath();
        ctx.moveTo(0, c.height * 0.5);
        ctx.lineTo(c.width, c.height * 0.5);
        ctx.stroke();

        drawLine(hr.red, "#e74c3c");
        drawLine(hr.blue, "#3498db");

        var r = hr.red[hr.red.length - 1];
        var b = hr.blue[hr.blue.length - 1];
        UI.rewardNumsEl.textContent = "Red " + r.toFixed(1) + " / Blue " + b.toFixed(1);
    };

    // Generic agent-stats panel. Agents that have a team-level planner
    // (portfolio search, influence maps, etc) publish stats into
    // state.agentStats and those are rendered here. Key/value pairs are
    // agent-defined; render verbatim.
    UI.updateAgentStats = function (stats) {
        if (!stats) {
            UI.agentStatsEl.textContent = "(no planner active)";
            return;
        }
        var lines = [];
        if (stats.label) lines.push(stats.label);
        for (var k in stats) {
            if (!Object.prototype.hasOwnProperty.call(stats, k)) continue;
            if (k === "label") continue;
            lines.push(k.padEnd(12) + String(stats[k]));
        }
        UI.agentStatsEl.textContent = lines.join("\n");
    };

    UI.setStatus = function (text) {
        UI.statusEl.textContent = text;
    };
})();
