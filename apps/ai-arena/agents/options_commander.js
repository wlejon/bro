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

    var frameActionsByTeam = {};

    function teamTick(appState, teamId) {
        var t = initTeam(teamId);
        var teamHeroes = (AI.shared.teams[teamId] || []).filter(function (h) {
            return h && h.unit && h.unit.alive;
        });
        if (!teamHeroes.length) {
            frameActionsByTeam[teamId] = {};
            return;
        }

        // Pass bound Agents to decide (which expects brogameagent Agent*).
        // AI.shared.teams[teamId] items ARE the bound agents — same objects
        // returned from bro.ai.game.createAgent in arena.js.
        var actions = t.commander.decide(AI.shared.world, teamHeroes);
        var byId = {};
        for (var i = 0; i < teamHeroes.length; i++) {
            byId[teamHeroes[i].unit.id] = actions[i];
        }
        frameActionsByTeam[teamId] = byId;
    }

    function think(self, world) {
        var u = self.agent.unit;
        if (!u.alive) { self.hold(0.5); return; }
        var frame = frameActionsByTeam[u.teamId];
        var action = frame ? frame[u.id] : null;
        if (!action) {
            // Commander hasn't planned yet this frame (first call / late
            // registration). Safe fallback.
            AI.think(self, world);
            return;
        }
        OptionsShared.applyCombatAction(self, world, action);
    }

    Agents.register({
        id: "options_commander",
        label: "Options-Commander (roles)",
        reset: function () {
            state = null;
            frameActionsByTeam = {};
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
