// agents/options_mcts.js — Single-hero OptionMcts agent.
//
// Each hero runs its own OptionMcts search over the TacticalOptions set.
// The search returns an option NAME; the agent then stays committed to
// that option until its should_terminate predicate fires or the option's
// max-window cap is hit. Search is therefore rare (once per option
// commit, not per tick) — option *execution* is just a cheap spec.step()
// call each think.
//
// Why it shouldn't thrash: the option layer compresses the decision
// space from ~18 CombatActions × N-deep tree to 8 options × (often) 2-3
// deep. A 100-iteration search at ~option_max=6 windows plans ~18
// windows of game time — enough to see past a peek-and-retreat
// consequence, which plain MCTS never could at the same budget.
//
// To tune: adjust iterations / budgetMs in the cfg below, or swap the
// evaluator to "teamAdvantage" via Commander (see options_commander.js).

(function () {
    "use strict";

    var built = null;        // { handles, specs, order, config }
    var mctsByHero = {};     // heroId -> OptionMctsHandle
    var memByHero  = {};     // heroId -> { option, ticks }

    function cfg() {
        return {
            iterations:       80,
            budgetMs:         3,
            rolloutHorizon:   3,
            actionRepeat:     2,
            optionMaxWindows: 6,
            useLeafValue:     true,
            seed:             0xAICAFE,
            opponentPolicy:   "scripted",
            evaluator:        "hpDelta",
        };
    }

    function ensureOptions() {
        if (built) return;
        built = TacticalOptions.build();
    }

    function mctsFor(heroId) {
        var m = mctsByHero[heroId];
        if (m) return m;
        var c = cfg();
        c.options = built.handles;
        m = bro.ai.game.createOptionMcts(c);
        mctsByHero[heroId] = m;
        return m;
    }

    function think(self, world) {
        ensureOptions();
        var agent = self.agent;
        var u = agent.unit;
        if (!u.alive) { self.hold(0.5); return; }

        var mem = memByHero[u.id] || (memByHero[u.id] = { option: null, ticks: 0 });
        var mcts = mctsFor(u.id);

        var selfView  = OptionsShared.viewAgent(agent);
        var worldView = OptionsShared.viewWorld();

        // Continue current option if its termination predicate is still
        // false. This keeps options committed for their natural lifetime
        // — the whole reason search is rare.
        var spec = mem.option ? built.specs[mem.option] : null;
        var terminated = !spec
            || spec.shouldTerminate(selfView, worldView, mem.ticks)
            || mem.ticks >= cfg().optionMaxWindows;

        if (terminated) {
            var chosen = mcts.search(world, agent);
            if (chosen) {
                mcts.advanceRoot(chosen);
                mem.option = chosen;
                mem.ticks = 0;
                spec = built.specs[chosen];
            } else {
                // No option can_initiate here — fall back to scripted.
                mem.option = null;
                mem.ticks = 0;
                AI.think(self, world);
                return;
            }
        }

        // Steady-state: just step the committed option.
        var action = spec.step(selfView, worldView, mem.ticks);
        OptionsShared.applyCombatAction(self, world, action);
        mem.ticks++;
    }

    Agents.register({
        id: "options_mcts",
        label: "Options-MCTS",
        reset: function () {
            mctsByHero = {};
            memByHero  = {};
            built = null;
        },
        think: think,
        stats: function () {
            // Count how many heroes are committed to each option — useful
            // debug signal when tuning.
            var counts = {};
            for (var id in memByHero) {
                var n = memByHero[id].option || "(none)";
                counts[n] = (counts[n] || 0) + 1;
            }
            var out = { label: "options_mcts" };
            for (var k in counts) out[k] = counts[k];
            return out;
        },
    });
})();
