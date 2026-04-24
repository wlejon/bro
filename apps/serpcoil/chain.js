// chain.js — the marching serpent.
//
// The chain is an ordered list of orbs, each with {color, d}, where `d` is
// distance-along-path. Orbs are kept packed by ORB_DIAM whenever possible
// (with small backlog when inserted orbs push the line back). The leading
// orb advances at `speed` px/s; trailing orbs are kept tightly packed
// behind it.
var SC = SC || {};

SC.Chain = (function () {
    "use strict";

    var ORB_DIAM = 30; // render & spacing diameter

    // COLORS: 6 palette slots. Index 1..6 are "real" colors; 0 reserved.
    var COLORS = [
        null,
        { hex: "#e63946", name: "crimson", tone: 523.25 }, // C5
        { hex: "#f4a261", name: "amber",   tone: 587.33 }, // D5
        { hex: "#e9c46a", name: "gold",    tone: 659.25 }, // E5
        { hex: "#2a9d8f", name: "teal",    tone: 783.99 }, // G5
        { hex: "#4cc9f0", name: "azure",   tone: 880.00 }, // A5
        { hex: "#b56dff", name: "violet",  tone: 1046.50 } // C6
    ];

    function lighten(hex, amt) {
        var r = parseInt(hex.slice(1,3),16), g = parseInt(hex.slice(3,5),16), b = parseInt(hex.slice(5,7),16);
        r = Math.min(255, r + amt); g = Math.min(255, g + amt); b = Math.min(255, b + amt);
        return "rgb("+r+","+g+","+b+")";
    }

    // Render a single orb at (x,y). The highlight shifts slightly based
    // on a per-orb phase so the chain shimmers as it moves.
    function drawOrb(ctx, x, y, color, phase) {
        var c = COLORS[color];
        if (!c) return;
        var r = ORB_DIAM / 2;
        // Outer ring
        ctx.fillStyle = "rgba(0,0,0,0.5)";
        ctx.beginPath();
        ctx.arc(x, y + 2, r, 0, Math.PI * 2);
        ctx.fill();
        // Base
        ctx.fillStyle = c.hex;
        ctx.beginPath();
        ctx.arc(x, y, r, 0, Math.PI * 2);
        ctx.fill();
        // Specular highlight
        var hx = x + Math.cos(phase) * r * 0.4 - r * 0.25;
        var hy = y + Math.sin(phase) * r * 0.4 - r * 0.3;
        ctx.fillStyle = lighten(c.hex, 120);
        ctx.globalAlpha = 0.55;
        ctx.beginPath();
        ctx.arc(hx, hy, r * 0.35, 0, Math.PI * 2);
        ctx.fill();
        ctx.globalAlpha = 1.0;
        // Thin outer ring
        ctx.strokeStyle = "rgba(255,255,255,0.28)";
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.arc(x, y, r - 0.5, 0, Math.PI * 2);
        ctx.stroke();
    }

    function create(opts) {
        opts = opts || {};
        var path = opts.path;
        var palette = opts.palette || [1, 2, 3];   // which color indices are in play
        var totalToSpawn = opts.totalToSpawn || 50;
        var speed = opts.speed || 35;              // px/s march speed
        var baseSpeed = speed;
        var insertSpeed = opts.insertSpeed || 1000; // px/s when inserting/settling
        var spawnInterval = opts.spawnInterval || 380; // ms between new orbs

        var orbs = [];  // {color, d, phase}
        var spawned = 0;
        var spawnTimer = 0;
        var dangerActive = false;
        var slowmoTimer = 0;      // ms remaining
        var speedBoostTimer = 0;  // ms scaling
        var rng = opts.rng || Math.random;

        // Utility: insert an orb into the array while keeping sort order by d.
        function insertSorted(orb) {
            var idx = 0;
            while (idx < orbs.length && orbs[idx].d < orb.d) idx++;
            orbs.splice(idx, 0, orb);
            return idx;
        }

        function randomColor() {
            return palette[(rng() * palette.length) | 0];
        }

        function spawnOrb(dtMs) {
            if (spawned >= totalToSpawn) return;
            spawnTimer += dtMs;
            if (spawnTimer < spawnInterval) return;
            spawnTimer -= spawnInterval;
            // Spawn at d just behind existing first orb so chain stays packed.
            var d0 = 0;
            if (orbs.length > 0) d0 = orbs[0].d - ORB_DIAM;
            orbs.unshift({ color: randomColor(), d: d0, phase: rng() * Math.PI * 2 });
            spawned++;
        }

        // March the chain forward. We move the head, then pack everyone
        // else behind it tightly.
        function advance(dtMs) {
            var dtS = dtMs / 1000;
            var spd = baseSpeed;
            if (slowmoTimer > 0) { spd *= 0.5; slowmoTimer -= dtMs; }
            // Danger speedup
            if (dangerActive) spd *= 1.25;

            // Advance head (last orb) forward.
            if (orbs.length === 0) return;
            var head = orbs[orbs.length - 1];
            head.d += spd * dtS;
            // Pack trailing orbs: each orb must be at least ORB_DIAM behind the next.
            for (var i = orbs.length - 2; i >= 0; i--) {
                var target = orbs[i+1].d - ORB_DIAM;
                if (orbs[i].d > target) orbs[i].d = target;
                // otherwise leave gaps (caused by recent pops) so chain snaps back.
                // Settle: pull orbs that are behind target forward toward the next.
                else if (orbs[i].d < target) {
                    // Shift forward at insertSpeed (simulates chain snap).
                    orbs[i].d = Math.min(target, orbs[i].d + (insertSpeed * dtS));
                }
            }
            // Prevent the first orb from going negative.
            if (orbs[0].d < 0) orbs[0].d = 0;
        }

        // Check if the lead orb has reached the goal.
        function headReachedGoal() {
            if (orbs.length === 0) return false;
            return orbs[orbs.length - 1].d >= path.length();
        }

        // Danger = lead orb in last X% of path.
        function updateDangerState() {
            var threshold = path.length() * 0.82;
            dangerActive = orbs.length > 0 && orbs[orbs.length - 1].d >= threshold;
            return dangerActive;
        }

        // Insert an orb into the chain at distance d (from a fired
        // projectile). Returns the insertion index.
        function insertAt(d, color) {
            var orb = { color: color, d: d, phase: rng() * Math.PI * 2 };
            // Find nearest index by distance.
            var idx = insertSorted(orb);
            // Push everything FORWARD from idx onward by ORB_DIAM so the
            // inserted orb doesn't overlap — simulates the chain making
            // room. Orbs BEHIND remain stationary; they'll snap-forward on
            // the next advance.
            for (var j = idx + 1; j < orbs.length; j++) {
                orbs[j].d = Math.max(orbs[j].d, orbs[j-1].d + ORB_DIAM);
            }
            return idx;
        }

        // Detect runs of 3+ matching colors starting the search at
        // `hintIdx`. Returns an array of [startIdx, endIdx) ranges.
        function detectMatches(hintIdx) {
            var ranges = [];
            if (orbs.length < 3) return ranges;
            // Only look at the single run that contains hintIdx (if given).
            if (hintIdx != null) {
                var color = orbs[hintIdx].color;
                var start = hintIdx, end = hintIdx;
                while (start > 0 && orbs[start-1].color === color) start--;
                while (end < orbs.length - 1 && orbs[end+1].color === color) end++;
                if (end - start + 1 >= 3) ranges.push([start, end + 1]);
                return ranges;
            }
            // Otherwise sweep the whole chain.
            var i = 0;
            while (i < orbs.length) {
                var c = orbs[i].color;
                var j = i;
                while (j < orbs.length && orbs[j].color === c) j++;
                if (j - i >= 3) ranges.push([i, j]);
                i = j;
            }
            return ranges;
        }

        // Remove orbs in [start, end). Returns the removed array.
        function removeRange(start, end) {
            var removed = orbs.splice(start, end - start);
            return removed;
        }

        // Pop matches around an index, chaining cascading pops. Returns
        // { popped: [...orbs], comboDepth: n, positions: [...xy] }.
        function popAround(hintIdx, onPop) {
            var totalPopped = [];
            var combo = 0;
            var positions = [];
            var idx = hintIdx;
            while (true) {
                var ranges = detectMatches(idx);
                if (ranges.length === 0) break;
                var r = ranges[0];
                // Record positions for particle fx.
                for (var k = r[0]; k < r[1]; k++) {
                    var p = path.pointAt(orbs[k].d);
                    positions.push({ x: p.x, y: p.y, color: orbs[k].color });
                }
                var rem = removeRange(r[0], r[1]);
                totalPopped = totalPopped.concat(rem);
                combo++;
                if (onPop) onPop(rem, combo, positions.slice(positions.length - rem.length));
                // After removal, orbs at r[0]-1 and r[0] (was r[1]) are now adjacent.
                // Check if they form a new match.
                var a = r[0] - 1, b = r[0];
                if (a >= 0 && b < orbs.length && orbs[a].color === orbs[b].color) {
                    idx = a;
                    // Continue cascade.
                } else {
                    break;
                }
            }
            return { popped: totalPopped, comboDepth: combo, positions: positions };
        }

        // Push orbs backward by `amount` (powerup: backtrack).
        function backtrack(amount) {
            for (var i = 0; i < orbs.length; i++) {
                orbs[i].d -= amount;
                if (orbs[i].d < 0) orbs[i].d = 0;
            }
            // Repack so we don't leave overlaps at d=0.
            for (var j = 1; j < orbs.length; j++) {
                if (orbs[j].d < orbs[j-1].d + ORB_DIAM) {
                    orbs[j].d = orbs[j-1].d + ORB_DIAM;
                }
            }
        }

        // Blaster: remove all orbs within `radius` of world point.
        function blastAt(wx, wy, radius) {
            var removed = [];
            var positions = [];
            var r2 = radius * radius;
            for (var i = orbs.length - 1; i >= 0; i--) {
                var p = path.pointAt(orbs[i].d);
                var dx = p.x - wx, dy = p.y - wy;
                if (dx*dx + dy*dy <= r2) {
                    positions.push({ x: p.x, y: p.y, color: orbs[i].color });
                    removed.push(orbs.splice(i, 1)[0]);
                }
            }
            return { popped: removed, positions: positions };
        }

        // Colorshift: turn orb at index + neighbors (same original color) to newColor.
        function colorshift(idx, newColor) {
            if (idx < 0 || idx >= orbs.length) return 0;
            var oldColor = orbs[idx].color;
            var start = idx, end = idx;
            while (start > 0 && orbs[start-1].color === oldColor) start--;
            while (end < orbs.length - 1 && orbs[end+1].color === oldColor) end++;
            for (var k = start; k <= end; k++) orbs[k].color = newColor;
            return end - start + 1;
        }

        function setSlowmo(ms) { slowmoTimer = Math.max(slowmoTimer, ms); }

        function draw(ctx) {
            // Draw back-to-front so the head (closest to goal) is on top.
            for (var i = 0; i < orbs.length; i++) {
                var o = orbs[i];
                if (o.d < -ORB_DIAM * 0.5) continue; // still in the mouth
                var p = path.pointAt(Math.max(0, o.d));
                o.phase += 0.05;
                drawOrb(ctx, p.x, p.y, o.color, o.phase);
            }
        }

        function remainingToSpawn() { return totalToSpawn - spawned; }
        function isComplete() {
            return spawned >= totalToSpawn && orbs.length === 0;
        }

        return {
            // state
            orbs: function () { return orbs; },
            path: function () { return path; },
            speed: function (v) { if (v != null) baseSpeed = v; return baseSpeed; },
            remainingToSpawn: remainingToSpawn,
            totalToSpawn: function () { return totalToSpawn; },
            spawnedCount: function () { return spawned; },
            count: function () { return orbs.length; },
            isComplete: isComplete,
            dangerActive: function () { return dangerActive; },
            headD: function () {
                return orbs.length ? orbs[orbs.length - 1].d : 0;
            },
            // ticks
            tick: function (dtMs) {
                spawnOrb(dtMs);
                advance(dtMs);
                updateDangerState();
            },
            // operations
            insertAt: insertAt,
            detectMatches: detectMatches,
            popAround: popAround,
            backtrack: backtrack,
            blastAt: blastAt,
            colorshift: colorshift,
            setSlowmo: setSlowmo,
            // rendering
            draw: draw,
            ORB_DIAM: ORB_DIAM,
            // test hook: force empty
            forceEmpty: function () { orbs.length = 0; spawned = totalToSpawn; },
            setPalette: function (p) { palette = p.slice(); }
        };
    }

    return { create: create, COLORS: COLORS, ORB_DIAM: ORB_DIAM };
})();
