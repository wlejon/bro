// guides.js — 5 themed guide characters with distinct green-peg abilities.
//
// A guide is a pure-data object plus a trigger function. The trigger is
// called when the ball strikes a green peg; it mutates world state (adds
// pegs, sets ball.onFire, etc.) and returns an object describing FX hints
// for the renderer to pick up.

'use strict';
(function (global) {
    const P = global.Physics;

    const GUIDES = [
        {
            id: 'wingtip', name: 'Wingtip', icon: '&#x2732;',
            color: '#67e1ff',
            blurb: 'Scatters a ring of extra bonus pegs on each green trigger.',
            trigger(world, peg) {
                const added = [];
                for (let i = 0; i < 8; i++) {
                    const ang = (i / 8) * Math.PI * 2;
                    const nx = peg.x + Math.cos(ang) * 60;
                    const ny = peg.y + Math.sin(ang) * 60;
                    // Avoid overlap with existing pegs.
                    if (world.pegs.some(q =>
                        !q.removed && Math.hypot(q.x - nx, q.y - ny) < 22)) continue;
                    if (nx < 30 || nx > P.FIELD_W - 30) continue;
                    if (ny < P.FIELD_TOP + 20 || ny > P.FIELD_BOTTOM - 60) continue;
                    P.addPeg(world, nx, ny, P.PEG.BLUE);
                    added.push(world.pegs[world.pegs.length - 1]);
                }
                return { fx: 'wingtip', added };
            },
        },
        {
            id: 'terraflame', name: 'Terraflame', icon: '&#x1F525;',
            color: '#ff7a3d',
            blurb: 'Ignites the ball so every peg within a glowing aura burns.',
            trigger(world, peg) {
                if (world.ball) world.ball.onFire = true;
                for (const b of world.extraBalls) b.onFire = true;
                return { fx: 'fire' };
            },
        },
        {
            id: 'pulsewave', name: 'Pulsewave', icon: '&#x25CE;',
            color: '#8fe07e',
            blurb: 'Emits a shockwave that lights every peg in a wide radius.',
            trigger(world, peg) {
                const cx = peg.x, cy = peg.y;
                const R = 160;
                // Queue every fresh peg in radius, sorted by distance so the
                // shock front lights them in order over the pulse duration.
                const queue = [];
                for (const p of world.pegs) {
                    if (p.removed || p.lit || p === peg) continue;
                    const dx = p.x - cx, dy = p.y - cy;
                    const d2 = dx * dx + dy * dy;
                    if (d2 > R * R) continue;
                    queue.push({ peg: p, dist: Math.sqrt(d2) });
                }
                queue.sort((a, b) => a.dist - b.dist);
                (world.pulses || (world.pulses = [])).push({
                    cx, cy, R, age: 0, duration: 0.5, queue,
                });
                return { fx: 'pulse', cx, cy, radius: R };
            },
        },
        {
            id: 'orbital', name: 'Orbital', icon: '&#x2218;',
            color: '#c48eff',
            blurb: 'Splits the ball into a trio of echoes for a brief window.',
            trigger(world, peg) {
                P.spawnSplitBalls(world);
                return { fx: 'split' };
            },
        },
        {
            id: 'mirage', name: 'Mirage', icon: '&#x25C7;',
            color: '#ffd24a',
            blurb: 'Reveals the projected ball path for the next shot.',
            trigger(world, peg) {
                // Actual reveal is handled by app.js checking the flag; this
                // trigger only raises the flag for the *next* shot.
                world.mirageNextShot = true;
                return { fx: 'mirage' };
            },
        },
    ];

    function byId(id) { return GUIDES.find(g => g.id === id) || GUIDES[0]; }

    global.Guides = { GUIDES, byId };
})(typeof window !== 'undefined' ? window : globalThis);
