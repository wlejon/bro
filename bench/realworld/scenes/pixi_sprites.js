// pixi.js: bunnymark. N sprites bouncing under gravity, five procedurally
// generated 26x37 textures (so batches switch textures), the seeded PRNG for
// every random draw. Bound by per-sprite JS (physics, transform updates) and
// the batcher's per-quad vertex packing.

import { pixiScene, hashMatrix } from '../lib/pixi_runner.js';
import { Hasher } from '../lib/harness.js';

function bunnyTexture(PIXI, hue) {
    const W = 26, H = 37;
    const px = new Uint8Array(W * H * 4);
    const c = [Math.round(255 * (0.5 + 0.5 * Math.cos(hue))), Math.round(255 * (0.5 + 0.5 * Math.cos(hue + 2.1))), Math.round(255 * (0.5 + 0.5 * Math.cos(hue + 4.2)))];
    for (let y = 0; y < H; y++) {
        for (let x = 0; x < W; x++) {
            const i = (y * W + x) * 4;
            const ear = y < 14 && (Math.abs(x - 8) < 3 || Math.abs(x - 18) < 3);
            const dx = (x - 13) / 12, dy = (y - 25) / 11;
            const body = dx * dx + dy * dy < 1;
            if (ear || body) {
                const shade = body ? 0.85 - 0.15 * dy : 0.8;
                px[i] = c[0] * shade; px[i + 1] = c[1] * shade; px[i + 2] = c[2] * shade; px[i + 3] = 255;
            }
        }
    }
    const source = new PIXI.BufferImageSource({ resource: px, width: W, height: H, alphaMode: 'premultiply-alpha-on-upload' });
    return new PIXI.Texture({ source });
}

pixiScene({
    name: 'pixi_sprites',
    defaults: { n: 10000 },
    create({ PIXI, n, rng, cfg, stage }) {
        const textures = [0, 1.2, 2.4, 3.6, 4.8].map((h) => bunnyTexture(PIXI, h));
        const container = new PIXI.Container();
        stage.addChild(container);
        const W = cfg.w, H = cfg.h, gravity = 0.5;
        const bunnies = [];
        for (let i = 0; i < n; i++) {
            const b = new PIXI.Sprite(textures[i % textures.length]);
            b.anchor.set(0.5, 1);
            b.x = rng() * W;
            b.y = rng() * H * 0.5;
            b.vx = rng() * 10 - 5;
            b.vy = rng() * 10 - 5;
            b.rotation = rng() * 0.6 - 0.3;
            b.scale.set(0.5 + rng() * 0.5);
            container.addChild(b);
            bunnies.push(b);
        }
        return {
            step() {
                for (let i = 0; i < bunnies.length; i++) {
                    const b = bunnies[i];
                    b.x += b.vx;
                    b.y += b.vy;
                    b.vy += gravity;
                    if (b.x > W) { b.vx *= -1; b.x = W; }
                    else if (b.x < 0) { b.vx *= -1; b.x = 0; }
                    if (b.y > H) {
                        b.vy *= -0.85;
                        b.y = H;
                        if (rng() > 0.5) b.vy -= rng() * 6;
                    } else if (b.y < 0) { b.vy = 0; b.y = 0; }
                }
            },
            state(detail) {
                // World transforms, and each sprite's four quad corners as the
                // batcher packs them: local bounds through the world matrix.
                const wt = new Hasher(), quad = new Hasher();
                const chunks = detail ? [] : null;
                let ch = detail ? new Hasher() : null;
                for (let i = 0; i < bunnies.length; i++) {
                    const b = bunnies[i];
                    const m = b.worldTransform;
                    hashMatrix(wt, m);
                    const bd = b.bounds;
                    const xs = [bd.minX, bd.maxX, bd.maxX, bd.minX], ys = [bd.minY, bd.minY, bd.maxY, bd.maxY];
                    for (let k = 0; k < 4; k++) {
                        quad.num(m.a * xs[k] + m.c * ys[k] + m.tx);
                        quad.num(m.b * xs[k] + m.d * ys[k] + m.ty);
                    }
                    if (ch) { hashMatrix(ch, m); if ((i + 1) % 64 === 0) { chunks.push(ch.f32); ch = new Hasher(); } }
                }
                const last = bunnies[bunnies.length - 1];
                const out = { sprites: bunnies.length, wt: wt.both, quad: quad.both, sample: last ? [last.x, last.y, last.vy] : null };
                if (chunks) { if (ch.count) chunks.push(ch.f32); out.chunks = chunks; }
                return out;
            },
        };
    },
});
