// levels.js — 15 hand-authored path shapes.
//
// Each level has:
//   controls    — array of {x,y} in a 1280x800 reference viewport
//   totalOrbs   — number of orbs to spawn
//   palette     — color-index array (1..6)
//   chainSpeed  — px/s base march speed
//   shooter     — {x,y} shooter position (defaults to center if omitted)
//   name        — short descriptor
//
// Paths are designed to use the full viewport and curl back so the
// shooter always has visible chain to target.
var SC = SC || {};

SC.Levels = (function () {
    "use strict";

    var W = 1280, H = 800;

    // Generators for common shapes.
    function zigZag(count, margin, opts) {
        opts = opts || {};
        var pts = [];
        var top = opts.top || 120;
        var bottom = opts.bottom || H - 140;
        var xPad = opts.xPad || 140;
        var dx = (W - xPad * 2) / (count - 1);
        for (var i = 0; i < count; i++) {
            pts.push({ x: xPad + i * dx, y: (i % 2 === 0) ? top : bottom });
        }
        return pts;
    }

    function spiral(centerX, centerY, loops, startR, endR, ptsPerLoop) {
        var pts = [];
        var total = loops * ptsPerLoop;
        for (var i = 0; i < total; i++) {
            var t = i / total;
            var r = startR + (endR - startR) * t;
            var a = t * loops * Math.PI * 2;
            pts.push({ x: centerX + Math.cos(a) * r, y: centerY + Math.sin(a) * r });
        }
        return pts;
    }

    function sCurve(segments) {
        var pts = [];
        for (var i = 0; i <= segments; i++) {
            var t = i / segments;
            var x = 140 + t * (W - 280);
            var y = H / 2 + Math.sin(t * Math.PI * 2) * (H * 0.32);
            pts.push({ x: x, y: y });
        }
        return pts;
    }

    function figureEight(cx, cy, rx, ry, segments) {
        var pts = [];
        for (var i = 0; i <= segments; i++) {
            var t = i / segments * Math.PI * 2;
            pts.push({
                x: cx + Math.sin(t) * rx,
                y: cy + Math.sin(t * 2) * ry * 0.5
            });
        }
        return pts;
    }

    // Each level definition.
    var LEVELS = [
        // 1 — long gentle S
        {
            name: "Waking Coil",
            controls: [{x:140,y:180},{x:380,y:260},{x:620,y:180},{x:880,y:280},{x:1100,y:220},
                       {x:1140,y:500},{x:860,y:620},{x:560,y:580},{x:260,y:640},{x:140,y:560}],
            totalOrbs: 38, palette: [1, 2, 3], chainSpeed: 28,
            shooter: { x: W/2, y: H/2 + 20 }
        },
        // 2 — wider S
        {
            name: "River Bend",
            controls: sCurve(10),
            totalOrbs: 45, palette: [1, 2, 3], chainSpeed: 30,
            shooter: { x: 380, y: 80 }
        },
        // 3 — zig zag
        {
            name: "Lightning Path",
            controls: zigZag(7, 120, { top: 160, bottom: H - 160, xPad: 140 }),
            totalOrbs: 48, palette: [1, 2, 3, 4], chainSpeed: 32,
            shooter: { x: W/2, y: H/2 }
        },
        // 4 — single spiral in
        {
            name: "Inward Gyre",
            controls: spiral(W/2, H/2, 2.2, 330, 80, 16),
            totalOrbs: 52, palette: [1, 2, 3, 4], chainSpeed: 30,
            shooter: { x: 1140, y: 140 }
        },
        // 5 — figure eight
        {
            name: "Twin Runes",
            controls: figureEight(W/2, H/2, 420, 260, 28),
            totalOrbs: 55, palette: [1, 2, 3, 4], chainSpeed: 32,
            shooter: { x: W/2, y: 80 }
        },
        // 6 — double S
        {
            name: "Switchback",
            controls: [{x:120,y:180},{x:1160,y:180},{x:1160,y:380},{x:120,y:380},
                       {x:120,y:580},{x:1160,y:580},{x:1160,y:700},{x:900,y:720}],
            totalOrbs: 58, palette: [1, 2, 3, 4], chainSpeed: 34,
            shooter: { x: 620, y: 480 }
        },
        // 7 — wide spiral
        {
            name: "Drawing Mandala",
            controls: spiral(W/2, H/2, 3, 360, 60, 18),
            totalOrbs: 62, palette: [1, 2, 3, 4], chainSpeed: 34,
            shooter: { x: 140, y: H - 140 }
        },
        // 8 — zigzag denser
        {
            name: "Sawtooth",
            controls: zigZag(9, 100, { top: 140, bottom: H - 140, xPad: 100 }),
            totalOrbs: 65, palette: [1, 2, 3, 4, 5], chainSpeed: 36,
            shooter: { x: W/2, y: H/2 }
        },
        // 9 — loop de loop
        {
            name: "Double Knot",
            controls: [{x:140,y:500},{x:320,y:260},{x:500,y:500},{x:380,y:720},{x:620,y:720},
                       {x:780,y:500},{x:960,y:260},{x:1140,y:500},{x:1020,y:720},{x:760,y:740}],
            totalOrbs: 68, palette: [1, 2, 3, 4, 5], chainSpeed: 36,
            shooter: { x: W/2, y: H/2 - 20 }
        },
        // 10 — spiral out + in
        {
            name: "Dual Gyre",
            controls: spiral(W/2 - 260, H/2, 1.8, 40, 220, 14)
                .concat([{x:W/2+10, y:H/2}])
                .concat(spiral(W/2 + 260, H/2, 1.8, 220, 40, 14)),
            totalOrbs: 72, palette: [1, 2, 3, 4, 5], chainSpeed: 36,
            shooter: { x: W/2, y: H - 160 }
        },
        // 11 — big S faster
        {
            name: "Serpent's Tongue",
            controls: sCurve(12),
            totalOrbs: 78, palette: [1, 2, 3, 4, 5], chainSpeed: 40,
            shooter: { x: W/2, y: H - 150 }
        },
        // 12 — tight figure 8
        {
            name: "Bound Infinity",
            controls: figureEight(W/2, H/2, 460, 260, 36),
            totalOrbs: 82, palette: [1, 2, 3, 4, 5, 6], chainSpeed: 40,
            shooter: { x: W/2, y: 80 }
        },
        // 13 — spiral tight
        {
            name: "Whorl of Thorns",
            controls: spiral(W/2, H/2, 3.5, 340, 50, 20),
            totalOrbs: 86, palette: [1, 2, 3, 4, 5, 6], chainSpeed: 42,
            shooter: { x: 1140, y: 660 }
        },
        // 14 — snake maze
        {
            name: "Labyrinth",
            controls: [
                {x:120,y:140},{x:1160,y:140},
                {x:1160,y:280},{x:220,y:280},
                {x:220,y:420},{x:1160,y:420},
                {x:1160,y:560},{x:220,y:560},
                {x:220,y:700},{x:1160,y:700}
            ],
            totalOrbs: 92, palette: [1, 2, 3, 4, 5, 6], chainSpeed: 42,
            shooter: { x: 80, y: 480 }
        },
        // 15 — final: complex spiral/figure8 hybrid
        {
            name: "Final Coil",
            controls: spiral(W/2, H/2, 2.5, 360, 80, 16)
                .concat(figureEight(W/2, H/2, 80, 60, 8)),
            totalOrbs: 100, palette: [1, 2, 3, 4, 5, 6], chainSpeed: 46,
            shooter: { x: 1200, y: 80 }
        }
    ];

    function count() { return LEVELS.length; }
    function get(idx) { return LEVELS[idx]; }

    // Scale level controls for current viewport (W0, H0).
    function scaled(idx, W0, H0) {
        var L = LEVELS[idx];
        var sx = W0 / 1280, sy = H0 / 800;
        var out = { name: L.name, totalOrbs: L.totalOrbs, palette: L.palette.slice(),
                    chainSpeed: L.chainSpeed,
                    controls: [],
                    shooter: {
                        x: (L.shooter ? L.shooter.x : 640) * sx,
                        y: (L.shooter ? L.shooter.y : 400) * sy
                    }};
        for (var i = 0; i < L.controls.length; i++) {
            out.controls.push({ x: L.controls[i].x * sx, y: L.controls[i].y * sy });
        }
        return out;
    }

    return { count: count, get: get, scaled: scaled, LEVELS: LEVELS };
})();
