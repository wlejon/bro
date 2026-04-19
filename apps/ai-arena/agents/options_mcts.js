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
            seed:             128,
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

    // Rough duration of each option in game seconds. Approximates the
    // max_windows × window_dt the option would consume inside MCTS if it
    // ran to its natural termination cap. Used to size queue entries so
    // the robot keeps executing an option for its intended lifetime
    // without the planner re-firing search every tick.
    // Kept short so MCTS re-votes often. Longer durations = less search,
    // which matters more than the planning-horizon advantage when the
    // per-search iteration budget is only ~80. If the search budget
    // grows (deeper trees, parallel search), raise these toward each
    // option's natural terminator (max 3–5 windows ≈ 1 s).
    var OPTION_DURATION = {
        holdAndFire:    0.4,
        advanceToRange: 0.6,
        retreatBack:    0.6,
        focusWeakest:   0.5,
        strafeFire:     0.5,
        selfHeal:       0.3,
        pokeFireball:   0.3,
        grenadeCluster: 0.3,
    };
    function durationFor(name) { return OPTION_DURATION[name] || 0.8; }

    function think(self, world) {
        ensureOptions();
        var agent = self.agent;
        var u = agent.unit;
        if (!u.alive) { self.hold(0.5); return; }

        var mem = memByHero[u.id] || (memByHero[u.id] = { lastThinkT: -1 });
        var mcts = mctsFor(u.id);

        // dt for Bot.tick (BotAim / cd decay / queue bookkeeping).
        var simT = AI.shared.simT;
        var prevT = mem.lastThinkT < 0 ? simT : mem.lastThinkT;
        var dt = Math.max(0.001, Math.min(0.2, simT - prevT));
        mem.lastThinkT = simT;

        // Preempt: if the currently-committed option's should_terminate
        // now fires (HP dropped, target lost, drifted out of the kite
        // band…), drop the queue so we re-search this tick. Without this
        // the robot would keep executing stale plans for up to
        // durationFor(option) seconds — fine for planning horizon but
        // bad for reactivity.
        if (mem.committed) {
            var spec = built.specs[mem.committed];
            if (spec) {
                var sv = OptionsShared.viewAgent(agent);
                var wv = OptionsShared.viewWorld();
                // Use elapsed-in-option as a window count approximation.
                if (spec.shouldTerminate(sv, wv, mem.ticksInOption || 0)) {
                    Bot.clear(self);
                    mem.committed = null;
                }
            }
        }
        mem.ticksInOption = (mem.ticksInOption || 0) + 1;

        // Plan: when the queue is empty, run MCTS to pick the next option
        // and push it as a single-entry plan with its natural duration.
        // The robot then executes autonomously for that duration (or
        // until preempted above). This is the "plan, then execute" split
        // — the planner isn't reacting every tick, it's voting on what
        // the robot does next.
        if (Bot.queueLength(self) === 0) {
            var chosen = mcts.search(world, agent);
            if (chosen) {
                mcts.advanceRoot(chosen);
                Bot.push(self, OptionsShared.robotCommandFor(chosen),
                    durationFor(chosen));
                mem.committed = chosen;
                mem.ticksInOption = 0;
            } else {
                mem.committed = null;
            }
            // Else: no option can_initiate — leave queue empty, robot
            // uses defaultCommand for this tick; we'll retry next tick.
        }

        Bot.tick(self, dt);
    }

    // Book-keeping hook: when a match resets, stats still want per-hero
    // commitment counts. We derive from Bot.current(self) now, but the
    // registered stats() runs at team level — easier to keep a mem field.
    // (Left as a TODO; stats still reads memByHero below.)

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
                var n = memByHero[id].committed || "(none)";
                counts[n] = (counts[n] || 0) + 1;
            }
            var out = { label: "options_mcts" };
            for (var k in counts) out[k] = counts[k];
            return out;
        },
    });
})();
