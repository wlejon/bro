// ghosts.js — record + render a small history of past trajectories.
//
// A trajectory is a list of frame snapshots: { tick, x, y, frame, facing }.
// Recorders are write-only during play, frozen on commit, then drawn at the
// caller's per-frame tick offset by Ghosts.draw().
//
// Usage:
//   <script src="../lib/ghosts.js"></script>
//   const ghosts = Ghosts.create({ maxGhosts: 6 });
//   const rec = ghosts.startRecording();
//   // each tick:
//   rec.record(tick, { x, y, frame, facing });
//   // on episode end (death/flag/timeout):
//   ghosts.commit(rec);
//   // each render frame:
//   ghosts.draw(ctx, currentTick, drawSpriteFn);
//     // drawSpriteFn(ctx, x, y, frame, facingFlipped, alpha) draws one ghost.

(function (global) {
    'use strict';

    function create(opts) {
        opts = opts || {};
        const maxGhosts = opts.maxGhosts || 6;
        // Ghosts are committed trajectories. Newest at end.
        const ghosts = [];

        function startRecording() {
            const frames = [];
            return {
                frames,
                record(tick, snap) {
                    frames.push({
                        tick:   tick,
                        x:      snap.x,
                        y:      snap.y,
                        frame:  snap.frame | 0,
                        facing: snap.facing | 0,
                    });
                },
            };
        }

        function commit(rec) {
            if (!rec || rec.frames.length === 0) return;
            ghosts.push({ frames: rec.frames, committedAt: performance.now() });
            while (ghosts.length > maxGhosts) ghosts.shift();
        }

        function clear() { ghosts.length = 0; }

        function count() { return ghosts.length; }

        // Draw all ghosts at `currentTick`. Older ghosts get lower alpha.
        // drawSpriteFn(ctx, x, y, frame, facingFlipped, alpha).
        function draw(ctx, currentTick, drawSpriteFn) {
            const n = ghosts.length;
            for (let gi = 0; gi < n; gi++) {
                const g = ghosts[gi];
                // Pick the frame whose tick is closest to currentTick (clamped to range).
                const f = lookupFrame(g.frames, currentTick);
                if (!f) continue;
                // Newest ghost = brightest. Oldest = barely visible.
                const ageRank = (gi + 1) / n;       // 0..1, oldest small
                const alpha = 0.10 + 0.30 * ageRank;
                drawSpriteFn(ctx, f.x, f.y, f.frame, f.facing < 0, alpha);
            }
        }

        // Binary search for the frame at-or-just-before `tick`.
        function lookupFrame(frames, tick) {
            if (frames.length === 0) return null;
            if (tick <= frames[0].tick) return frames[0];
            if (tick >= frames[frames.length - 1].tick) return frames[frames.length - 1];
            let lo = 0, hi = frames.length - 1;
            while (hi - lo > 1) {
                const mid = (lo + hi) >> 1;
                if (frames[mid].tick <= tick) lo = mid; else hi = mid;
            }
            return frames[lo];
        }

        return { startRecording, commit, clear, count, draw };
    }

    global.Ghosts = { create };
})(typeof window !== 'undefined' ? window : globalThis);
