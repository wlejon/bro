// agents/options_commander.js — Role-based team planner.
//
// Three roles partitioned across the team:
//   lead     — frontline aggression: advance, strafe-fire, hold-and-fire,
//              focus-weakest, grenadeCluster.
//   flank    — pick off vulnerable enemies: focusWeakest, advance,
//              pokeFireball, strafeFire. Prioritises low-HP targets
//              but won't commit to the main push.
//   support  — preserve the team: selfHeal, retreatBack, pokeFireball,
//              holdAndFire. Falls back to kiting when healthy.
//
// The custom assigner picks roles from live state each replan window:
//   - HP < 40% → support
//   - HP < 70% → flank
//   - else     → lead
// Round-robin among same-tier heroes to keep the ratio reasonable.
//
// One OptionMcts per hero (per-role option set). Commander triggers
// re-search only when a hero's committed option terminates.

(function () {
    "use strict";

    var state = null;   // per-team state, populated lazily
    // state[teamId] = {
    //   built: TacticalOptions build result,
    //   commander: AICommander handle,
    //   memByHero: { heroId -> {option, ticks} },
    // }

    function initTeam(teamId) {
        if (state && state[teamId]) return state[teamId];
        state = state || {};

        var built = TacticalOptions.build();
        var s = built.specs;

        // Collect handle subsets by name. Using names to pick from specs
        // so we stay in sync with tactical_options.js ordering.
        function handlesFor(names) {
            var out = [];
            for (var i = 0; i < names.length; i++) {
                // Each createOption call inside build() is one-shot — we
                // rebuild a fresh handle here so every role gets its own
                // set (Commander can't share option pointers across roles).
                out.push(bro.ai.game.createOption({
                    name: names[i],
                    canInitiate:     s[names[i]].canInitiate,
                    step:            s[names[i]].step,
                    shouldTerminate: s[names[i]].shouldTerminate,
                }));
            }
            return out;
        }

        var leadOpts = handlesFor([
            "advanceToRange", "strafeFire", "holdAndFire",
            "focusWeakest", "grenadeCluster",
        ]);
        var flankOpts = handlesFor([
            "focusWeakest", "advanceToRange", "pokeFireball",
            "strafeFire", "retreatBack",
        ]);
        var supportOpts = handlesFor([
            "selfHeal", "retreatBack", "pokeFireball",
            "holdAndFire", "strafeFire",
        ]);

        var roleCfg = {
            iterations: 60, budgetMs: 3, rolloutHorizon: 3,
            actionRepeat: 2, optionMaxWindows: 6, useLeafValue: true,
            seed: 0xABCDEF,
        };

        var commander = bro.ai.game.createCommander({
            replanEveryWindows: 4,
            roleCfg: roleCfg,
            opponentPolicy: "scripted",
            evaluator: "hpDelta",
            roles: [
                { name: "lead",    options: leadOpts },
                { name: "flank",   options: flankOpts },
                { name: "support", options: supportOpts,
                  // Support values HP preservation over damage dealt.
                  evaluator: function (worldView, heroId) {
                      var me = null;
                      for (var i = 0; i < worldView.agents.length; i++) {
                          if (worldView.agents[i].id === heroId) { me = worldView.agents[i]; break; }
                      }
                      if (!me) return 0;
                      var mine = 0, mineMax = 0, enemy = 0, enemyMax = 0;
                      for (var k = 0; k < worldView.agents.length; k++) {
                          var a = worldView.agents[k];
                          if (a.teamId === me.teamId) { mine += a.hp; mineMax += a.maxHp; }
                          else                         { enemy += a.hp; enemyMax += a.maxHp; }
                      }
                      var mineF  = mineMax  > 0 ? mine  / mineMax  : 0;
                      var enemyF = enemyMax > 0 ? enemy / enemyMax : 0;
                      // Weight ally HP 2× enemy HP delta.
                      return Math.max(-1, Math.min(1, (mineF - enemyF) * 1.5 + (mineF - 0.5)));
                  } },
            ],
            // Live-state role assignment. Runs at Commander's replan
            // cadence, not per tick.
            assign: function (heroes, world) {
                var out = [];
                for (var i = 0; i < heroes.length; i++) {
                    var h = heroes[i];
                    if (!h || !h.alive) { out.push(0); continue; }
                    var f = h.maxHp > 0 ? h.hp / h.maxHp : 0;
                    if (f < 0.40)      out.push(2); // support
                    else if (f < 0.70) out.push(1); // flank
                    else               out.push(0); // lead
                }
                return out;
            },
        });

        var t = { built: built, commander: commander, memByHero: {} };
        state[teamId] = t;
        return t;
    }

    // Commander.decide plans for the whole team in one call — but the
    // ai-arena think loop calls per-agent think. We run decide once per
    // rAF frame in teamTick and cache the result; per-agent think then
    // just applies the precomputed action for that hero.

    function teamTick(appState, teamId) {
        var t = initTeam(teamId);
        var teamHeroes = (AI.shared.teams[teamId] || []).filter(function (h) {
            return h && h.unit && h.unit.alive;
        });
        if (!teamHeroes.length) {
            t.lastHeroes = [];
            return;
        }

        // Pass bound Agents to decide (which expects brogameagent Agent*).
        // We don't actually use the returned actions — execution goes
        // through Bot.tick via per-hero think. But decide() still needs to
        // run each frame so the Commander advances option commitments and
        // re-plans roles on schedule. Cache the hero ordering so think()
        // can map heroId → committedOption(idx).
        t.commander.decide(AI.shared.world, teamHeroes);
        t.lastHeroes = teamHeroes.slice();
    }

    // Per-hero tracking: last simT (for Bot dt) and last option the
    // commander committed us to (so we know when to replace the queue).
    var heroMem = {};

    // Natural duration per option. Same table as options_mcts; the
    // commander replans internally so we don't need exact numbers —
    // the queue gets replaced whenever Commander swings to a new
    // option. We just want each push big enough that a short tick
    // doesn't expire it immediately.
    var OPTION_DURATION = {
        holdAndFire: 0.8, advanceToRange: 1.4, retreatBack: 1.4,
        focusWeakest: 1.1, strafeFire: 1.1, selfHeal: 0.5,
        pokeFireball: 0.4, grenadeCluster: 0.4,
    };
    function durationFor(name) { return OPTION_DURATION[name] || 0.8; }

    function think(self, world) {
        var u = self.agent.unit;
        if (!u.alive) { self.hold(0.5); return; }

        var teamState = state && state[u.teamId];
        var name = teamState ? committedOptionName(teamState, u.id) : null;

        var hm = heroMem[u.id] || (heroMem[u.id] = { lastT: -1, lastOption: null });

        // Queue maintenance: when Commander swings to a new option (or
        // assigns one after a re-plan), drop whatever the robot was doing
        // and install the new intent. Otherwise the queue keeps running —
        // Bot.tick handles per-frame execution autonomously.
        if (name && name !== hm.lastOption) {
            Bot.replace(self, [{
                cmd: OptionsShared.robotCommandFor(name),
                duration: durationFor(name),
            }]);
            hm.lastOption = name;
        } else if (!name && Bot.queueLength(self) === 0) {
            // Commander hasn't assigned anyone yet (first frame) — let the
            // robot's default engage command apply until Commander catches up.
            hm.lastOption = null;
        }

        var simT = AI.shared.simT;
        var prevT = hm.lastT < 0 ? simT : hm.lastT;
        var dt = Math.max(0.001, Math.min(0.2, simT - prevT));
        hm.lastT = simT;

        Bot.tick(self, dt);
    }

    // Look up the Commander's currently-committed option name for a given
    // hero id. Commander exposes committed_option_for_hero(idx); we need
    // the assignment vector to map heroId → idx.
    function committedOptionName(teamState, heroId) {
        var c = teamState.commander;
        if (!c || typeof c.committedOption !== "function") return null;
        // Commander.decide was called with teamHeroes at index time; match
        // our hero to that same ordering.
        var heroes = teamState.lastHeroes || [];
        for (var i = 0; i < heroes.length; i++) {
            if (heroes[i] && heroes[i].unit && heroes[i].unit.id === heroId) {
                var name = c.committedOption(i);
                return name || null;
            }
        }
        return null;
    }

    function heroTeam(heroId) {
        var teams = AI.shared.teams || [[], []];
        for (var t = 0; t < teams.length; t++) {
            for (var i = 0; i < teams[t].length; i++) {
                if (teams[t][i].unit.id === heroId) return t;
            }
        }
        return 0;
    }

    Agents.register({
        id: "options_commander",
        label: "Options-Commander (roles)",
        reset: function () {
            state = null;
            heroMem = {};
        },
        teamTick: teamTick,
        think: think,
        stats: function (appState, teamId) {
            if (!state || !state[teamId]) return null;
            var c = state[teamId].commander;
            var roles = c.roles;
            var assigns = c.currentAssignments;
            var counts = { lead: 0, flank: 0, support: 0 };
            for (var i = 0; i < assigns.length; i++) {
                var r = roles[assigns[i]];
                if (r) counts[r.name] = (counts[r.name] || 0) + 1;
            }
            return {
                label: "options_commander",
                lead:             counts.lead    || 0,
                flank:            counts.flank   || 0,
                support:          counts.support || 0,
                windowsUntilReplan: c.windowsUntilReplan,
            };
        },
    });
})();
