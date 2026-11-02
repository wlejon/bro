// pixi.js: N Graphics objects, each cleared and redrawn every frame with an
// animated shape (circle, rect, rounded rect, star, ellipse, polygon) filled
// and stroked. Bound by GraphicsContext rebuilding: path building,
// triangulation (earcut) and stroke extrusion, then upload.

import { pixiScene, hashMatrix } from '../lib/pixi_runner.js';
import { Hasher } from '../lib/harness.js';

pixiScene({
    name: 'pixi_graphics',
    defaults: { n: 1000 },
    create({ PIXI, n, rng, cfg, stage, stateMode }) {
        const cols = Math.max(1, Math.ceil(Math.sqrt(n * cfg.w / cfg.h)));
        const cell = cfg.w / cols;
        const items = [];
        for (let i = 0; i < n; i++) {
            const g = new PIXI.Graphics();
            g.x = (i % cols + 0.5) * cell;
            g.y = (Math.floor(i / cols) + 0.5) * cell;
            stage.addChild(g);
            items.push({ g, kind: i % 6, phase: rng() * Math.PI * 2, fill: Math.floor(rng() * 0xffffff), line: Math.floor(rng() * 0xffffff) });
        }
        const gpuCtx = new PIXI.GpuGraphicsContext();
        const geo = { v: new Hasher(), i: new Hasher(), uv: new Hasher(), batches: 0 };
        let hashing = false;
        // What GraphicsContextSystem.updateGpuContext does for a batchable
        // context: buildContextBatches into GPU-side geometry arrays.
        const build = (ctx) => {
            gpuCtx.reset();
            PIXI.buildContextBatches(ctx, gpuCtx);
            if (hashing) {
                const d = gpuCtx.geometryData;
                geo.v.arr(d.vertices); geo.i.arr(d.indices); geo.uv.arr(d.uvs);
                geo.batches += gpuCtx.batches.length;
            }
        };
        const draw = (it, t) => {
            const g = it.g, r = cell * (0.3 + 0.1 * Math.sin(t * 2 + it.phase));
            g.clear();
            switch (it.kind) {
                case 0: g.circle(0, 0, r); break;
                case 1: g.rect(-r, -r * 0.7, r * 2, r * 1.4); break;
                case 2: g.roundRect(-r, -r, r * 2, r * 2, r * 0.3); break;
                case 3: g.star(0, 0, 5, r, r * 0.45, t + it.phase); break;
                case 4: g.ellipse(0, 0, r, r * 0.6); break;
                default: {
                    const pts = [];
                    for (let k = 0; k < 7; k++) { const a = k / 7 * Math.PI * 2 + t; pts.push(Math.cos(a) * r * (k % 2 ? 0.6 : 1), Math.sin(a) * r * (k % 2 ? 0.6 : 1)); }
                    g.poly(pts);
                }
            }
            g.fill({ color: it.fill, alpha: 0.85 });
            g.stroke({ width: 2, color: it.line });
            g.rotation = t * 0.5 + it.phase;
        };
        return {
            step(f, t) {
                for (let i = 0; i < items.length; i++) draw(items[i], t);
                if (stateMode) for (let i = 0; i < items.length; i++) build(items[i].g.context);
            },
            state() {
                geo.v = new Hasher(); geo.i = new Hasher(); geo.uv = new Hasher(); geo.batches = 0;
                hashing = true;
                for (let i = 0; i < items.length; i++) build(items[i].g.context);
                hashing = false;
                const wt = new Hasher();
                for (let i = 0; i < items.length; i++) hashMatrix(wt, items[i].g.worldTransform);
                const bh = new Hasher();
                for (let i = 0; i < items.length; i++) { const b = items[i].g.context.bounds; bh.num(b.minX); bh.num(b.minY); bh.num(b.maxX); bh.num(b.maxY); }
                return { shapes: items.length, wt: wt.both, verts: geo.v.both, idx: geo.i.exact + ':' + geo.i.count, uvs: geo.uv.both, batches: geo.batches, bounds: bh.both };
            },
        };
    },
});
