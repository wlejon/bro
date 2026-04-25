// levels.js — 20 hand-designed peg layouts for Pegbounce.
//
// A level is described by a recipe applied to a blank world. Each level
// chooses background gradient stops, a ball count, a set of peg-placing
// patterns, and scoring tiers (star thresholds). Peg positions use a
// 32-wide / 22-tall grid over the 1024x720 playfield (top band reserved
// for cannon). Grid cell is 32 px.
//
// Level recipes return an object:
//   { id, name, background, balls, stars: [bronze, silver, gold],
//     build(world, rng) }
// The build fn adds pegs via Physics.addPeg / addMovingPeg.

'use strict';
(function (global) {
    const P = global.Physics;
    const GRID_COLS = 32;
    const CELL = 32;
    const MARGIN_X = 16;          // ensures first column is at x=32
    const TOP = P.FIELD_TOP + 40; // first playable row below cannon

    function gx(c) { return MARGIN_X + c * CELL; }
    function gy(r) { return TOP + r * CELL; }

    // Bump oranges upward by this many per level late in the set, so
    // difficulty ramps.
    function place(world, c, r, type) {
        if (c < 0 || c >= GRID_COLS) return;
        const x = gx(c), y = gy(r);
        if (x < 20 || x > P.FIELD_W - 20) return;
        if (y < P.FIELD_TOP + 24 || y > P.FIELD_BOTTOM - 80) return;
        P.addPeg(world, x, y, type);
    }

    // Lay down a rectangular grid of pegs, optionally with a pattern mask.
    // `pattern(c, r)` returns a peg type string or null for empty.
    function grid(world, c0, r0, w, h, pattern) {
        for (let r = 0; r < h; r++) {
            for (let c = 0; c < w; c++) {
                const type = pattern(c, r);
                if (type) place(world, c0 + c, r0 + r, type);
            }
        }
    }

    function pyramid(world, cx, r0, rows, picker) {
        for (let i = 0; i < rows; i++) {
            const width = rows - i;
            for (let k = 0; k < width; k++) {
                const col = cx - (width - 1) + k * 2;
                place(world, col, r0 + i, picker(k, i, width));
            }
        }
    }

    function ring(world, cx, cy, radiusCells, count, picker) {
        for (let i = 0; i < count; i++) {
            const t = (i / count) * Math.PI * 2;
            const col = Math.round(cx + Math.cos(t) * radiusCells);
            const row = Math.round(cy + Math.sin(t) * radiusCells);
            place(world, col, row, picker(i, count));
        }
    }

    function line(world, c0, r0, dc, dr, count, picker) {
        for (let i = 0; i < count; i++) {
            place(world, c0 + dc * i, r0 + dr * i, picker(i, count));
        }
    }

    // Always-orange: picker helper.
    const allOrange = () => P.PEG.ORANGE;
    const allBlue = () => P.PEG.BLUE;

    function everyNth(n, type, other) {
        return (c, r, ctx) => (((c + r) % n) === 0 ? type : other);
    }

    // ---- Level definitions -----------------------------------------------
    const LEVELS = [
        // 1: Classic pyramid.
        {
            id: 1, name: 'First Bounce',
            background: ['#0a1230', '#041025'], balls: 10,
            stars: [1500, 3000, 6000],
            build(world) {
                pyramid(world, 16, 3, 7, (k, i) => (i === 3 && (k % 2 === 0)) ? P.PEG.ORANGE : P.PEG.BLUE);
                place(world, 16, 2, P.PEG.GREEN);
                place(world, 4, 12, P.PEG.PURPLE); place(world, 28, 12, P.PEG.PURPLE);
            }
        },
        // 2: Grid with scattered orange.
        {
            id: 2, name: 'Lattice',
            background: ['#0d0a30', '#02061a'], balls: 10,
            stars: [3000, 6000, 12000],
            build(world) {
                grid(world, 3, 4, 26, 8, (c, r) => (c % 2 === 0 && r % 2 === 0) ? (((c * 3 + r * 7) % 6 === 0) ? P.PEG.ORANGE : P.PEG.BLUE) : null);
                place(world, 8, 10, P.PEG.GREEN); place(world, 24, 10, P.PEG.GREEN);
                place(world, 16, 14, P.PEG.PURPLE);
            }
        },
        // 3: Double pyramid.
        {
            id: 3, name: 'Twin Peaks',
            background: ['#0e1340', '#050a24'], balls: 10,
            stars: [1500, 3500, 7000],
            build(world) {
                pyramid(world, 9, 4, 5, (k, i) => (i < 2 ? P.PEG.ORANGE : P.PEG.BLUE));
                pyramid(world, 23, 4, 5, (k, i) => (i < 2 ? P.PEG.ORANGE : P.PEG.BLUE));
                place(world, 16, 10, P.PEG.GREEN);
                place(world, 16, 14, P.PEG.PURPLE);
            }
        },
        // 4: V funnel.
        {
            id: 4, name: 'Funnel',
            background: ['#180a30', '#0a041e'], balls: 10,
            stars: [1200, 3000, 6000],
            build(world) {
                for (let i = 0; i < 10; i++) {
                    place(world, 4 + i, 3 + i, P.PEG.BLUE);
                    place(world, 28 - i, 3 + i, P.PEG.BLUE);
                }
                for (let i = 0; i < 6; i++) {
                    place(world, 11 + i, 13, (i % 2) ? P.PEG.ORANGE : P.PEG.BLUE);
                }
                place(world, 14, 8, P.PEG.GREEN); place(world, 18, 8, P.PEG.GREEN);
                place(world, 16, 15, P.PEG.PURPLE);
            }
        },
        // 5: Spiral.
        {
            id: 5, name: 'Spiral',
            background: ['#05242b', '#01161b'], balls: 9,
            stars: [2500, 5000, 9000],
            build(world) {
                let cx = 16, cy = 9;
                let r = 1;
                let n = 0;
                for (let step = 0; step < 72; step++) {
                    const ang = step * 0.48;
                    const col = Math.round(cx + Math.cos(ang) * r);
                    const row = Math.round(cy + Math.sin(ang) * r * 0.7);
                    const type = (n++ % 5 === 0) ? P.PEG.ORANGE : P.PEG.BLUE;
                    place(world, col, row, type);
                    r += 0.15;
                }
                place(world, 16, 9, P.PEG.GREEN);
                place(world, 4, 14, P.PEG.PURPLE); place(world, 28, 14, P.PEG.PURPLE);
            }
        },
        // 6: Rings.
        {
            id: 6, name: 'Ringmaster',
            background: ['#2a0530', '#140218'], balls: 9,
            stars: [2000, 4000, 8000],
            build(world) {
                ring(world, 16, 8,  3, 10, (i) => (i % 2) ? P.PEG.ORANGE : P.PEG.BLUE);
                ring(world, 16, 8,  5, 16, (i) => (i % 3 === 0) ? P.PEG.ORANGE : P.PEG.BLUE);
                ring(world, 16, 12, 7, 20, (i) => P.PEG.BLUE);
                place(world, 16, 8, P.PEG.GREEN);
                place(world, 16, 16, P.PEG.PURPLE);
            }
        },
        // 7: Wall with gap.
        {
            id: 7, name: 'Breakthrough',
            background: ['#0a3013', '#031508'], balls: 9,
            stars: [800, 1800, 3500],
            build(world) {
                for (let c = 2; c < 30; c++) {
                    if (c === 14 || c === 15 || c === 16 || c === 17) continue;
                    place(world, c, 6, P.PEG.BLUE);
                    place(world, c, 10, P.PEG.ORANGE);
                    place(world, c, 14, P.PEG.BLUE);
                }
                place(world, 15, 11, P.PEG.GREEN); place(world, 17, 11, P.PEG.GREEN);
                place(world, 2, 12, P.PEG.PURPLE); place(world, 30, 12, P.PEG.PURPLE);
            }
        },
        // 8: Checkerboard.
        {
            id: 8, name: 'Checkerboard',
            background: ['#201028', '#0a0414'], balls: 9,
            stars: [8000, 16000, 28000],
            build(world) {
                for (let r = 3; r <= 15; r++) {
                    for (let c = 2; c <= 30; c++) {
                        if (((c + r) & 1) === 0) {
                            const orange = ((c * 5 + r * 3) % 7) === 0;
                            place(world, c, r, orange ? P.PEG.ORANGE : P.PEG.BLUE);
                        }
                    }
                }
                place(world, 10, 5, P.PEG.GREEN); place(world, 22, 5, P.PEG.GREEN);
                place(world, 16, 15, P.PEG.PURPLE);
            }
        },
        // 9: Hourglass.
        {
            id: 9, name: 'Hourglass',
            background: ['#2a1a0a', '#140a03'], balls: 9,
            stars: [1500, 3500, 7000],
            build(world) {
                for (let i = 0; i < 10; i++) {
                    place(world, 4 + i, 3 + i, P.PEG.BLUE);
                    place(world, 28 - i, 3 + i, P.PEG.BLUE);
                    place(world, 4 + i, 18 - i, P.PEG.ORANGE);
                    place(world, 28 - i, 18 - i, P.PEG.ORANGE);
                }
                place(world, 16, 10, P.PEG.GREEN);
                place(world, 16, 15, P.PEG.PURPLE);
            }
        },
        // 10: Tight cluster.
        {
            id: 10, name: 'Cluster Bomb',
            background: ['#23053a', '#0e021a'], balls: 8,
            stars: [800, 1800, 3500],
            build(world) {
                grid(world, 10, 6, 13, 7, (c, r) => {
                    if ((c * 3 + r * 5) % 4 === 0) return P.PEG.ORANGE;
                    return P.PEG.BLUE;
                });
                place(world, 16, 5, P.PEG.GREEN);
                place(world, 16, 14, P.PEG.PURPLE); place(world, 8, 4, P.PEG.PURPLE);
            }
        },
        // 11: Moving orbits.
        {
            id: 11, name: 'Orbiters',
            background: ['#05204a', '#020f24'], balls: 8,
            stars: [1000, 2200, 4500],
            build(world) {
                grid(world, 3, 4, 26, 7, (c, r) => (r % 2 === 0 && c % 2 === 0) ? P.PEG.BLUE : null);
                for (let k = 0; k < 5; k++) {
                    const x = 150 + k * 180;
                    P.addMovingPeg(world, x, 380, P.PEG.ORANGE, 'orbit',
                        { radius: 40, speed: 1.6 + k * 0.1 });
                }
                place(world, 16, 4, P.PEG.GREEN);
                place(world, 2, 14, P.PEG.PURPLE); place(world, 30, 14, P.PEG.PURPLE);
            }
        },
        // 12: Slow walls.
        {
            id: 12, name: 'Tides',
            background: ['#0b1a3c', '#04091e'], balls: 8,
            stars: [800, 1800, 3500],
            build(world) {
                for (let r = 4; r <= 14; r += 2) {
                    for (let c = 4; c <= 28; c += 4) {
                        const type = (r === 8 || r === 12) ? P.PEG.ORANGE : P.PEG.BLUE;
                        P.addMovingPeg(world, gx(c), gy(r), type, 'oscillate',
                            { amp: 34, axis: (r % 4 === 0) ? 'x' : 'y', speed: 1.2 });
                    }
                }
                place(world, 16, 3, P.PEG.GREEN); place(world, 16, 15, P.PEG.PURPLE);
            }
        },
        // 13: Cascading steps.
        {
            id: 13, name: 'Cascade',
            background: ['#3a1205', '#170602'], balls: 8,
            stars: [2500, 5000, 9000],
            build(world) {
                for (let i = 0; i < 10; i++) {
                    for (let k = 0; k < 4; k++) {
                        place(world, 3 + i * 3 + k, 3 + i, (k === 0 || k === 3) ? P.PEG.ORANGE : P.PEG.BLUE);
                    }
                }
                place(world, 16, 4, P.PEG.GREEN);
                place(world, 16, 12, P.PEG.PURPLE);
            }
        },
        // 14: Honeycomb.
        {
            id: 14, name: 'Honeycomb',
            background: ['#3a2a05', '#170f02'], balls: 8,
            stars: [8000, 14000, 22000],
            build(world) {
                for (let r = 3; r <= 14; r++) {
                    const off = (r & 1) ? 1 : 0;
                    for (let c = 2 + off; c <= 30; c += 2) {
                        const type = ((c * 7 + r * 3) % 6 === 0) ? P.PEG.ORANGE : P.PEG.BLUE;
                        place(world, c, r, type);
                    }
                }
                place(world, 16, 3, P.PEG.GREEN); place(world, 8, 14, P.PEG.GREEN);
                place(world, 24, 14, P.PEG.PURPLE);
            }
        },
        // 15: Zigzag corridor.
        {
            id: 15, name: 'Zigzag',
            background: ['#05281e', '#02140e'], balls: 7,
            stars: [1200, 2800, 5500],
            build(world) {
                const rows = [
                    [2,3,4,5,6,7,8,9,10,11,12,13,14,15],
                    [18,19,20,21,22,23,24,25,26,27,28,29,30],
                    [2,3,4,5,6,7,8,9,10,11,12,13,14,15,16],
                    [17,18,19,20,21,22,23,24,25,26,27,28,29,30],
                ];
                for (let r = 0; r < rows.length; r++) {
                    for (const c of rows[r]) {
                        place(world, c, 4 + r * 3, ((c + r) % 3 === 0) ? P.PEG.ORANGE : P.PEG.BLUE);
                    }
                }
                place(world, 16, 13, P.PEG.GREEN);
                place(world, 4, 16, P.PEG.PURPLE); place(world, 28, 16, P.PEG.PURPLE);
            }
        },
        // 16: Orange core.
        {
            id: 16, name: 'Hot Core',
            background: ['#3a0518', '#180208'], balls: 7,
            stars: [2000, 4000, 8000],
            build(world) {
                ring(world, 16, 9, 7, 24, () => P.PEG.BLUE);
                ring(world, 16, 9, 4, 14, () => P.PEG.ORANGE);
                ring(world, 16, 9, 2,  8, () => P.PEG.ORANGE);
                place(world, 16, 9, P.PEG.GREEN);
                place(world, 4, 14, P.PEG.PURPLE); place(world, 28, 14, P.PEG.PURPLE);
            }
        },
        // 17: Gauntlet.
        {
            id: 17, name: 'Gauntlet',
            background: ['#081028', '#03060f'], balls: 7,
            stars: [1500, 3000, 6000],
            build(world) {
                for (let r = 3; r < 16; r += 2) {
                    for (let c = 2; c <= 30; c += 2) {
                        const type = (c === 2 || c === 30) ? P.PEG.BLUE
                                    : ((c * 5 + r * 3) % 7 === 0 ? P.PEG.ORANGE : P.PEG.BLUE);
                        place(world, c, r, type);
                    }
                }
                P.addMovingPeg(world, gx(16), gy(5),  P.PEG.GREEN, 'oscillate', { amp: 180, axis: 'x', speed: 1.4 });
                P.addMovingPeg(world, gx(16), gy(10), P.PEG.GREEN, 'oscillate', { amp: 180, axis: 'x', speed: 1.2 });
                place(world, 16, 15, P.PEG.PURPLE);
            }
        },
        // 18: Sparse + movers.
        {
            id: 18, name: 'Dance Floor',
            background: ['#1c062a', '#0a0213'], balls: 7,
            stars: [400, 1000, 2500],
            build(world) {
                for (let i = 0; i < 12; i++) {
                    const row = 4 + (i % 4) * 3;
                    const col = 3 + (i * 5) % 26;
                    place(world, col, row, P.PEG.BLUE);
                }
                for (let k = 0; k < 6; k++) {
                    P.addMovingPeg(world, 140 + k * 140, 280 + (k % 2) * 150, P.PEG.ORANGE, 'orbit',
                        { radius: 50, speed: 1.4 + k * 0.1 });
                }
                place(world, 16, 4, P.PEG.GREEN); place(world, 16, 15, P.PEG.PURPLE);
            }
        },
        // 19: Densest grid.
        {
            id: 19, name: 'Thickets',
            background: ['#220f03', '#0d0601'], balls: 7,
            stars: [10000, 18000, 28000],
            build(world) {
                for (let r = 3; r <= 16; r++) {
                    for (let c = 2; c <= 30; c++) {
                        if ((c + r) % 2 === 0) continue;
                        const type = ((c * 11 + r * 7) % 5 === 0) ? P.PEG.ORANGE : P.PEG.BLUE;
                        place(world, c, r, type);
                    }
                }
                place(world, 16, 3, P.PEG.GREEN); place(world, 4, 15, P.PEG.GREEN);
                place(world, 28, 15, P.PEG.GREEN);
                place(world, 16, 16, P.PEG.PURPLE);
            }
        },
        // 20: Final boss.
        {
            id: 20, name: 'The Maw',
            background: ['#3a0505', '#180202'], balls: 6,
            stars: [5000, 10000, 18000],
            build(world) {
                // Outer ring of blues.
                ring(world, 16, 9, 10, 36, () => P.PEG.BLUE);
                // Inner arcs of orange.
                ring(world, 16, 9, 7, 22, () => P.PEG.ORANGE);
                ring(world, 16, 9, 4, 14, () => P.PEG.ORANGE);
                // Kinetic defenders.
                for (let k = 0; k < 4; k++) {
                    P.addMovingPeg(world, gx(16), gy(9), P.PEG.ORANGE, 'orbit',
                        { radius: 40 + k * 18, speed: 1.2 + k * 0.2 });
                }
                place(world, 16, 9, P.PEG.GREEN);
                place(world, 4, 15, P.PEG.PURPLE); place(world, 28, 15, P.PEG.PURPLE);
            }
        },
    ];

    // Build helper: instantiate a world and apply the level's build fn.
    function buildLevel(levelIdx, seed) {
        const world = P.createWorld();
        world.rng = P.rand(seed || (1 + levelIdx * 37));
        const lv = LEVELS[levelIdx];
        lv.build(world, world.rng);
        return world;
    }

    global.Levels = { LEVELS, buildLevel, gx, gy, CELL, GRID_COLS, TOP };
})(typeof window !== 'undefined' ? window : globalThis);
