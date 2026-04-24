// path.js — Catmull-Rom spline sampled into a uniform arc-length table.
//
// A Path owns control points in screen space and builds a dense polyline
// samples so that `pointAt(d)` and `tangentAt(d)` are O(log n) in
// distance-along-path. Orbs in the chain store a scalar `d` and render
// by converting to (x,y) via this module.
var SC = SC || {};

SC.Path = (function () {
    "use strict";

    // Catmull-Rom interpolation of 4 points at parameter t in [0,1].
    function catmull(p0, p1, p2, p3, t) {
        var t2 = t * t, t3 = t2 * t;
        return {
            x: 0.5 * ((2 * p1.x) +
                     (-p0.x + p2.x) * t +
                     (2*p0.x - 5*p1.x + 4*p2.x - p3.x) * t2 +
                     (-p0.x + 3*p1.x - 3*p2.x + p3.x) * t3),
            y: 0.5 * ((2 * p1.y) +
                     (-p0.y + p2.y) * t +
                     (2*p0.y - 5*p1.y + 4*p2.y - p3.y) * t2 +
                     (-p0.y + 3*p1.y - 3*p2.y + p3.y) * t3)
        };
    }

    function create(controlPts, opts) {
        opts = opts || {};
        var samplesPerSeg = opts.samplesPerSeg || 32;
        var cps = controlPts.slice();
        // Duplicate endpoints so Catmull-Rom has valid neighbors on the ends.
        var pts = [cps[0]].concat(cps).concat([cps[cps.length - 1]]);

        var samples = []; // [{x,y,d}]
        var totalLen = 0;
        var prev = null;
        for (var i = 0; i < pts.length - 3; i++) {
            for (var s = 0; s < samplesPerSeg; s++) {
                var t = s / samplesPerSeg;
                var p = catmull(pts[i], pts[i+1], pts[i+2], pts[i+3], t);
                if (prev) {
                    var dx = p.x - prev.x, dy = p.y - prev.y;
                    totalLen += Math.sqrt(dx*dx + dy*dy);
                }
                samples.push({ x: p.x, y: p.y, d: totalLen });
                prev = p;
            }
        }
        // Append the final endpoint.
        var last = cps[cps.length - 1];
        if (prev) {
            var dx2 = last.x - prev.x, dy2 = last.y - prev.y;
            totalLen += Math.sqrt(dx2*dx2 + dy2*dy2);
        }
        samples.push({ x: last.x, y: last.y, d: totalLen });

        function pointAt(d) {
            if (d <= 0) return { x: samples[0].x, y: samples[0].y };
            if (d >= totalLen) {
                var e = samples[samples.length - 1];
                return { x: e.x, y: e.y };
            }
            // Binary search for d in samples.
            var lo = 0, hi = samples.length - 1;
            while (lo + 1 < hi) {
                var m = (lo + hi) >> 1;
                if (samples[m].d <= d) lo = m; else hi = m;
            }
            var a = samples[lo], b = samples[hi];
            var span = b.d - a.d;
            var t = span > 0 ? (d - a.d) / span : 0;
            return { x: a.x + (b.x - a.x) * t, y: a.y + (b.y - a.y) * t };
        }

        function tangentAt(d) {
            var eps = 4;
            var p1 = pointAt(Math.max(0, d - eps));
            var p2 = pointAt(Math.min(totalLen, d + eps));
            var dx = p2.x - p1.x, dy = p2.y - p1.y;
            var len = Math.sqrt(dx*dx + dy*dy) || 1;
            return { x: dx / len, y: dy / len };
        }

        // Nearest point on path to screen (px,py). Returns {d, dist}.
        function nearestTo(px, py) {
            var bestD = 0, bestDist = Infinity;
            // First sweep at sample resolution.
            for (var i = 0; i < samples.length; i++) {
                var dx = samples[i].x - px, dy = samples[i].y - py;
                var dd = dx*dx + dy*dy;
                if (dd < bestDist) { bestDist = dd; bestD = samples[i].d; }
            }
            return { d: bestD, dist: Math.sqrt(bestDist) };
        }

        return {
            length: function () { return totalLen; },
            pointAt: pointAt,
            tangentAt: tangentAt,
            nearestTo: nearestTo,
            samples: samples,
            controlPoints: cps,
            // Draw the path trench.
            draw: function (ctx, opts2) {
                opts2 = opts2 || {};
                ctx.save();
                ctx.lineCap = "round";
                ctx.lineJoin = "round";
                // Outer soft glow
                ctx.beginPath();
                for (var i = 0; i < samples.length; i++) {
                    if (i === 0) ctx.moveTo(samples[i].x, samples[i].y);
                    else         ctx.lineTo(samples[i].x, samples[i].y);
                }
                ctx.strokeStyle = "rgba(70,40,120,0.35)";
                ctx.lineWidth = (opts2.width || 36) + 14;
                ctx.stroke();
                // Trench body
                ctx.strokeStyle = "rgba(16,10,32,0.9)";
                ctx.lineWidth = opts2.width || 36;
                ctx.stroke();
                // Inner highlight stripe
                ctx.strokeStyle = "rgba(100,60,180,0.18)";
                ctx.lineWidth = 2;
                ctx.stroke();
                ctx.restore();
            }
        };
    }

    return { create: create };
})();
