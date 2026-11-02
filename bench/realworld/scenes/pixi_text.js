// pixi.js: N Text objects whose content changes every frame (a counter and a
// word from a fixed list), four styles. Bound by text measurement, canvas
// glyph rasterisation and the per-text texture upload.

import { pixiScene, hashMatrix } from '../lib/pixi_runner.js';
import { Hasher } from '../lib/harness.js';

const WORDS = ['alpha', 'bravo', 'charlie', 'delta', 'echo', 'foxtrot', 'golf', 'hotel', 'india', 'juliet'];

pixiScene({
    name: 'pixi_text',
    defaults: { n: 200 },
    create({ PIXI, n, rng, cfg, stage }) {
        const styles = [
            new PIXI.TextStyle({ fontFamily: 'sans-serif', fontSize: 14, fill: 0xffffff }),
            new PIXI.TextStyle({ fontFamily: 'sans-serif', fontSize: 18, fill: 0xffd060, fontWeight: 'bold' }),
            new PIXI.TextStyle({ fontFamily: 'serif', fontSize: 16, fill: 0x80d0ff, stroke: { color: 0x000000, width: 2 } }),
            new PIXI.TextStyle({ fontFamily: 'monospace', fontSize: 12, fill: 0xa0ffa0 }),
        ];
        const cols = Math.max(1, Math.ceil(Math.sqrt(n * cfg.w / cfg.h / 4)));
        const cw = cfg.w / cols, chh = Math.max(14, cw / 4);
        const texts = [];
        for (let i = 0; i < n; i++) {
            const t = new PIXI.Text({ text: 'init', style: styles[i % styles.length] });
            t.x = (i % cols) * cw + 4;
            t.y = Math.floor(i / cols) * chh + 2;
            t.rotation = (rng() - 0.5) * 0.1;
            stage.addChild(t);
            texts.push({ t, k: Math.floor(rng() * 1000), w: Math.floor(rng() * WORDS.length) });
        }
        return {
            step(f) {
                for (let i = 0; i < texts.length; i++) {
                    const it = texts[i];
                    it.t.text = WORDS[(it.w + f) % WORDS.length] + ' ' + ((it.k + f * 7) % 1000);
                }
            },
            state() {
                const wt = new Hasher(), bd = new Hasher(), content = new Hasher();
                for (let i = 0; i < texts.length; i++) {
                    const t = texts[i].t;
                    hashMatrix(wt, t.worldTransform);
                    const b = t.bounds;
                    bd.num(b.minX); bd.num(b.minY); bd.num(b.maxX); bd.num(b.maxY);
                    content.str(t.text);
                }
                return { texts: texts.length, wt: wt.both, bounds: bd.both, content: content.exact };
            },
        };
    },
});
