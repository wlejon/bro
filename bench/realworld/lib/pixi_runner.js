// pixi.js v8 scene runner. A scene module calls
//   pixiScene({ name, defaults, create(ctx) -> { step(f, t), state(detail) } })
// In state mode there is no renderer: the stage is a render group and
// updateRenderGroupTransforms() (what RenderGroupSystem runs each render)
// computes the transforms; text is measured by a fixed-advance stub so node
// and bronze measure alike. In render modes an Application renders to the
// harness's canvas; in mode=bench that canvas is lib/bench_env.js's stub and
// pixi's DOMAdapter makes stub canvases too.

// bench_env first: it defines `navigator` where the engine has none (a
// bronze-built executable) before pixi reads it at module level.
import { installPixiBench } from './bench_env.js';
import * as PIXI from '../vendor/pixi.mjs';
import { run, makeRng, Hasher } from './harness.js';

// A canvas whose 2D context measures every glyph as 0.55 em wide, ascent
// 0.8 em, descent 0.2 em: deterministic, engine-independent.
function stubMeasureCanvas() {
    const ctx = {
        font: '10px sans-serif', letterSpacing: '0px', textLetterSpacing: '0px',
        measureText(s) {
            const m = /(\d+(?:\.\d+)?)px/.exec(this.font);
            const px = m ? Number(m[1]) : 10;
            const w = String(s).length * px * 0.55;
            return { width: w, actualBoundingBoxLeft: 0, actualBoundingBoxRight: w, actualBoundingBoxAscent: px * 0.8, actualBoundingBoxDescent: px * 0.2, fontBoundingBoxAscent: px * 0.8, fontBoundingBoxDescent: px * 0.2 };
        },
    };
    return { width: 10, height: 10, getContext() { return ctx; } };
}

export function hashMatrix(h, m) { h.num(m.a); h.num(m.b); h.num(m.c); h.num(m.d); h.num(m.tx); h.num(m.ty); }

export function pixiScene(def) {
    run({
        name: def.name,
        lib: 'pixi',
        defaults: def.defaults,
        async start(cfg, h) {
            const rng = makeRng(cfg.seed);
            let app = null, stage, renderer = null;
            if (h) {
                // mode=bench: stub canvases and text measurement (lib/bench_env.js)
                if (h.bench) installPixiBench(PIXI);
                app = new PIXI.Application();
                await app.init({
                    canvas: h.canvas, width: cfg.w, height: cfg.h, preference: 'webgl', antialias: !!cfg.aa,
                    resolution: 1, autoDensity: false, backgroundColor: 0x101018, autoStart: false,
                    sharedTicker: false, hello: false, powerPreference: 'high-performance',
                });
                stage = app.stage;
                renderer = app.renderer;
            } else {
                stage = new PIXI.Container({ isRenderGroup: true });
                PIXI.CanvasTextMetrics.__canvas = stubMeasureCanvas();
                PIXI.CanvasTextMetrics.__context = null;
                PIXI.CanvasTextMetrics._experimentalLetterSpacingSupported = false;
            }
            const s = await def.create({ PIXI, n: cfg.n, rng, cfg, stage, renderer, stateMode: !h });
            return {
                info() {
                    const gl = renderer.gl;
                    return { lib: 'pixi', version: PIXI.VERSION, rendererType: renderer.type, glVersion: renderer.context.webGLVersion, maxTextures: renderer.limits.maxBatchableTextures, ctxAttrs: gl.getContextAttributes() };
                },
                frame(f) {
                    s.step(f, f / 60);
                    if (renderer) renderer.render(stage);
                    else PIXI.updateRenderGroupTransforms(stage.renderGroup, true);
                },
                state(detail) {
                    const out = s.state(detail);
                    const b = stage.getBounds();
                    const bh = new Hasher();
                    bh.num(b.minX); bh.num(b.minY); bh.num(b.maxX); bh.num(b.maxY);
                    out.stageBounds = bh.both;
                    return out;
                },
                gl() { return renderer.gl; },
            };
        },
    });
}

export { PIXI };
