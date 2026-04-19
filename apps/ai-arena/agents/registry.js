// agents/registry.js — Registry of AI algorithms selectable per team.
//
// Each entry:
//   id         short id used in state.redAi / state.blueAi and the selector
//   label      UI label
//   think      (self, world) => void — per-agent think called by AgentBinding
//              at thinkHz. Receives the bound `self` which wraps the agent
//              plus its capability set.
//   teamTick?  (state, teamId, dt) => void — optional team-level planner
//              called each rAF frame before per-agent think. Write plan
//              state into per-agent memory; think reads it. Use this for
//              algorithms that need global search (portfolio, influence
//              maps, etc.) rather than purely reactive per-unit logic.
//   stats?     (state, teamId) => { label, ... } — optional stats block
//              for the AGENT STATS panel. Returned keys render verbatim.
//
// Register at script-load time; main.js / controls.js / loop.js look up
// by id each frame so hot-swapping via the UI selector works without
// re-attaching bindings.
var Agents = {};
(function () {
    "use strict";

    var byId = {};
    var ordered = [];

    Agents.register = function (def) {
        if (!def || !def.id || typeof def.think !== "function") {
            console.warn("Agents.register: invalid definition", def);
            return;
        }
        if (byId[def.id]) {
            console.warn("Agents.register: duplicate id " + def.id);
            return;
        }
        byId[def.id] = def;
        ordered.push(def);
    };

    Agents.get = function (id) { return byId[id] || null; };
    Agents.all = function () { return ordered.slice(); };

    // Dispatch per team. The AgentBinding calls think via `self`; we route
    // to the registered agent for the unit's team based on state.redAi /
    // state.blueAi. Missing/unknown ids fall back to the first registered
    // agent (scripted baseline) so UI and state are never out of sync.
    Agents.thinkFor = function (self, world) {
        var teamId = self.agent.unit.teamId;
        var state = (typeof App !== "undefined" && App.state) ? App.state : null;
        var id = state ? (teamId === 0 ? state.redAi : state.blueAi) : null;
        var def = byId[id] || ordered[0];
        if (def) def.think(self, world);
    };

    // Called once per rAF frame by main.js. Each team's active agent may
    // optionally run a team-level planner before per-agent thinks fire.
    Agents.tickTeams = function (state, dt) {
        for (var teamId = 0; teamId < 2; teamId++) {
            var id = teamId === 0 ? state.redAi : state.blueAi;
            var def = byId[id];
            if (def && def.teamTick) def.teamTick(state, teamId, dt);
        }
    };

    // Reset any closure-held state across all agents. Called between
    // matches in fast_eval so stale per-agent memories (influence dest
    // caches, portfolio planner commits, etc.) don't leak into the next
    // match's early ticks.
    Agents.resetAll = function () {
        for (var i = 0; i < ordered.length; i++) {
            if (ordered[i].reset) ordered[i].reset();
        }
    };

    // Collect stats from whichever team's agent opts in. Blue takes
    // priority (since most planners will be blue during A/B); red falls
    // through if blue's agent doesn't expose stats.
    Agents.collectStats = function (state) {
        var teams = [1, 0];
        for (var i = 0; i < teams.length; i++) {
            var id = teams[i] === 0 ? state.redAi : state.blueAi;
            var def = byId[id];
            if (def && def.stats) {
                var s = def.stats(state, teams[i]);
                if (s) return s;
            }
        }
        return null;
    };
})();
