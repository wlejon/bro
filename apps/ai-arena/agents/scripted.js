// agents/scripted.js — The hand-tuned scripted policy from ai.js. Serves
// both as a baseline opponent and as the "inside" model that rollout-based
// agents (portfolio search) use to simulate what the enemy team will do.
//
// The actual algorithm lives in AI.think (ai.js). This file just wraps it
// as a registered agent so both teams can pick it from the selectors.
(function () {
    "use strict";

    Agents.register({
        id: "scripted",
        label: "Scripted",
        think: function (self, world) { AI.think(self, world); },
    });
})();
