// agents/random.js — Demonstrates Bot's per-channel queues by feeding each
// channel (move / aim / fire / cast) a stream of independent, randomly
// sampled actions. The four queues advance on their own clocks so the bot
// looks one direction, walks somewhere unrelated, fires when its aim
// happens to land on a target, and occasionally lobs a random ability —
// all in parallel.
//
// This isn't meant to be competitive. It exists to (a) verify the channel
// queues actually run independently, and (b) provide a baseline opponent
// that exercises every code path Bot supports without any tactical bias.
//
// Top-up policy: when any channel falls below MIN_DEPTH entries, refill
// it back up to FILL_TO with fresh random partials. With short per-entry
// durations the bot churns through ~10 different actions per channel
// every few seconds — clearly random to the eye, and clearly NOT bundled
// (you'll see the bot aim east while walking north while firing south).

(function () {
    "use strict";

    var MIN_DEPTH = 3;
    var FILL_TO = 10;          // Bot.MAX_QUEUE; keep in sync if it changes
    var ENTRY_DURATION = 0.4;  // seconds each random partial holds the channel

    function pick(arr) { return arr[(Math.random() * arr.length) | 0]; }

    var MOVE_MODES = ["hold", "advance", "retreat", "strafe", "moveTo"];
    var AIM_POLICIES = ["nearest", "focus", "weakest"];

    function randomMove() {
        var mode = pick(MOVE_MODES);
        var p = { mode: mode, space: true, kiteBand: [0.45, 0.85] };
        if (mode === "moveTo") {
            // Arena half-extent ≈ 19 (matches Bot.handleMove's clamp).
            p.x = -18 + Math.random() * 36;
            p.z = -18 + Math.random() * 36;
        }
        return p;
    }

    // 65% of the time pick a target policy (so we'll occasionally fire
    // when the gun lines up); 35% pick a free-look yaw and scan blindly.
    function randomAim() {
        if (Math.random() < 0.65) {
            return { policy: pick(AIM_POLICIES), requireLOS: false };
        }
        return { lookYaw: -Math.PI + Math.random() * 2 * Math.PI };
    }

    // Fire is binary on/off per entry. Bot still gates on aim alignment
    // and cooldown, so an "on" entry just enables firing — it doesn't
    // force a shot.
    function randomFire() { return Math.random() < 0.7; }

    // 25% of cast entries actually queue an ability; the rest are nulls
    // (no autocast for the duration). When set, target rotates between
    // focus / self / cluster so every cfg branch in maybeCast gets hit.
    function randomCast() {
        if (Math.random() > 0.25) return null;
        var slot = (Math.random() * 5) | 0;
        var allow = {};
        allow[slot] = { target: pick(["focus", "self", "cluster"]) };
        return allow;
    }

    function topUpChannel(self, channel, gen) {
        if (Bot.channelLength(self, channel) >= MIN_DEPTH) return;
        while (Bot.channelLength(self, channel) < FILL_TO) {
            Bot.pushChannel(self, channel, gen(), ENTRY_DURATION);
        }
    }

    Agents.register({
        id: "random",
        label: "Random",
        think: function (self /*, world*/) {
            var u = self.agent.unit;
            if (!u.alive) { self.hold(0.5); return; }
            var mem = AI.getMem(u.id);
            var simT = AI.shared.simT;
            var prevT = mem.lastThinkT < 0 ? simT : mem.lastThinkT;
            var dt = Math.max(0.001, Math.min(0.2, simT - prevT));
            mem.lastThinkT = simT;

            topUpChannel(self, "move", randomMove);
            topUpChannel(self, "aim",  randomAim);
            topUpChannel(self, "fire", randomFire);
            topUpChannel(self, "cast", randomCast);

            Bot.tick(self, dt);
        },
    });
})();
