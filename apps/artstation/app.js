// Art Station — driven from headless via globals defined at the bottom.
//
// Workflow:
//   1. Author an asset module under assets/<name>.js that calls
//      defineSheet(...) or defineTileset(...).
//   2. From headless: load('<name>'); render(); save('<name>'); preview(...);
//   3. Output PNG lives in apps/artstation/output/<name>.png and is ready
//      to feed back into scene.createSprite / createTilemap.

(function () {

    const sheetCanvas   = document.getElementById('sheet');
    const stageCanvas   = document.getElementById('stage');
    const stage3dCanvas = document.getElementById('stage-3d');
    const status        = document.getElementById('status');
    const info          = document.getElementById('info');

    // Live-preview scene context for `scene` assets. Built lazily on first
    // scene preview, then reused — clearScene() wipes child nodes between
    // assets without losing the GL context. Distinct from captureCanvas:
    // this one is on-screen and composited by the engine each tick (no
    // captureFrame/readback in the live path); captureCanvas stays
    // off-screen for export (renderScene/saveVideo/saveGif).
    let stage3dScene = null;
    function ensureStage3D() {
        if (!stage3dScene) stage3dScene = stage3dCanvas.getContext('scene');
        return stage3dScene;
    }
    function showStage3D(on) {
        stage3dCanvas.style.display = on ? 'block' : 'none';
        stageCanvas.style.display   = on ? 'none'  : 'block';
    }

    // Registry of every asset defined by loaded modules.
    const REGISTRY = {};         // name -> { kind: 'sheet'|'tileset', spec }
    let CURRENT = null;          // currently-rendered asset name

    // ---------- Definition API (called from asset modules) ---------------

    // spec: { frameWidth, frameHeight, cols, rows, frames, animations?, bg? }
    //   - frames: array of (ctx, w, h, frameIndex) => void
    //   - cols * rows must be >= frames.length
    //   - bg defaults to transparent
    function defineSheet(name, spec) {
        REGISTRY[name] = { kind: 'sheet', spec };
    }

    // spec: { tileSize, cols, tiles, bg? }
    //   - tiles: array; index 0 is reserved (engine uses 0 = empty)
    //   - tile entries are (ctx, size, tileIndex) => void; null = skip cell
    function defineTileset(name, spec) {
        REGISTRY[name] = { kind: 'tileset', spec };
    }

    // spec: { width, height, draw, bg?, pixel? }
    //   - draw: (ctx, w, h) => void
    //   - Single static image. Sidecar manifest stays minimal.
    function defineImage(name, spec) {
        REGISTRY[name] = { kind: 'image', spec };
    }

    // spec: { width, height, slice: {left, right, top, bottom}, draw, bg?, pixel? }
    //   - draw: (ctx, w, h) => void
    //   - Nine-slice image. Manifest carries slice rects so game code can
    //     stretch the middle and tile the edges.
    function defineNineSlice(name, spec) {
        if (!spec.slice) throw new Error('defineNineSlice requires slice {left,right,top,bottom}');
        REGISTRY[name] = { kind: 'nineslice', spec };
    }

    // spec: { regions, padding?, maxWidth?, pixel?, bg? }
    //   - regions: array of { name, width, height, draw }
    //     draw: (ctx, w, h, name) => void
    //   - padding: gutter pixels between regions (default 1)
    //   - maxWidth: bin width for the shelf-pack (default 256)
    //   - Pack variable-sized regions into one PNG with a JSON region map.
    //     Region name → {x, y, w, h} so game code can crop subimages without
    //     a fixed grid.
    function defineAtlas(name, spec) {
        if (!Array.isArray(spec.regions)) throw new Error('defineAtlas requires regions array');
        REGISTRY[name] = { kind: 'atlas', spec };
    }

    // spec: { frameWidth, frameHeight, fps, duration, cols, init?, frame,
    //         animations?, bg?, pixel? }
    //   - fps * duration = frame count; cols sets sheet layout (rows auto).
    //   - init() => state. Called once before frame 0. Optional; default {}.
    //   - frame(ctx, w, h, t, dt, state): called once per frame. ctx is
    //     pre-translated to the cell so coords are local (0..w, 0..h).
    //     t = i / fps (seconds, starts at 0); dt = 1 / fps (constant).
    //   - animations: same shape as defineSheet. If omitted, a default
    //     'play' animation is generated covering all frames at `fps`.
    //   - Output is a regular spritesheet PNG — game code can feed it
    //     into scene.createSprite without knowing it was procedural.
    function defineAnimated(name, spec) {
        if (typeof spec.frame !== 'function') {
            throw new Error('defineAnimated requires frame(ctx,w,h,t,dt,state) function');
        }
        if (!spec.fps || !spec.duration) {
            throw new Error('defineAnimated requires fps and duration');
        }
        REGISTRY[name] = { kind: 'animated', spec };
    }

    // spec: { frameWidth, frameHeight, fps, duration, cols, build, frame,
    //         animations?, bg?, pixel? }
    //   - build(scene) => refs. Called once on a fresh offscreen scene canvas
    //     to populate meshes / lights / camera. Return value is passed back
    //     into frame() so per-frame code can grab nodes without globals.
    //   - frame(scene, t, dt, refs, frameIndex): called once per frame to
    //     animate transforms / drive physics / etc. The framework renders the
    //     scene after frame() returns and copies the resulting pixels into
    //     the i-th cell of the sheet canvas via getImageData/putImageData.
    //   - Output is a regular sprite-sheet PNG (manifest kind='sheet') so
    //     scene.createSprite, saveVideo, saveGif, and preview() all work.
    //   - This is the 3D pipeline: a real scene with PBR lighting + bromesh
    //     geometry rendered through the engine's mesh FBO + tonemap, captured
    //     each timestep into a 2D atlas. Use it when canvas-2D painting can't
    //     reach the lighting/geometry quality you need.
    function defineScene(name, spec) {
        if (typeof spec.build !== 'function') {
            throw new Error('defineScene requires build(scene) function');
        }
        if (typeof spec.frame !== 'function') {
            throw new Error('defineScene requires frame(scene,t,dt,refs,i) function');
        }
        if (!spec.fps || !spec.duration) {
            throw new Error('defineScene requires fps and duration');
        }
        REGISTRY[name] = { kind: 'scene', spec };
    }

    function listAssets() {
        return Object.keys(REGISTRY).map(n => ({
            name: n, kind: REGISTRY[n].kind
        }));
    }

    // ---------- Loading -------------------------------------------------

    // Read assets/<name>.js from disk and eval it in the global scope.
    // Synchronous and idempotent (each load() reruns the module so edits
    // pick up without restarting headless). Uses brokit fs which is
    // available in both windowed and headless modes.
    function load(name) {
        const fs = require('fs');
        // Asset path is resolved relative to the app dir. brokit fs
        // honors the engine's app cwd for relative reads.
        const src = fs.readFileSync('assets/' + name + '.js', 'utf8');
        // Indirect eval -> global scope, so `defineSheet` / `brush` /
        // any top-level vars resolve via the window globals app.js set up.
        (0, eval)(src);
        if (!REGISTRY[name]) {
            throw new Error(`assets/${name}.js loaded but did not register "${name}"`);
        }
        CURRENT = name;
        status.textContent = `loaded ${name} (${REGISTRY[name].kind})`;
        return REGISTRY[name];
    }

    // ---------- Rendering ------------------------------------------------

    function sheetSize(spec) {
        return {
            w: spec.frameWidth * spec.cols,
            h: spec.frameHeight * spec.rows,
        };
    }

    function tilesetSize(spec) {
        const rows = Math.ceil(spec.tiles.length / spec.cols);
        return { w: spec.tileSize * spec.cols, h: spec.tileSize * rows };
    }

    function animatedFrameCount(spec) {
        return Math.max(1, Math.round(spec.fps * spec.duration));
    }

    function animatedSize(spec) {
        const n = animatedFrameCount(spec);
        const cols = spec.cols || Math.min(n, 8);
        const rows = Math.ceil(n / cols);
        return { w: spec.frameWidth * cols, h: spec.frameHeight * rows, cols, rows, n };
    }

    // Shelf-pack: simple bin-packing for variable-sized rectangles. Sorts by
    // height descending, then fills each shelf left-to-right, opening a new
    // shelf when current width is exceeded. Good packing density for atlases
    // of similarly-sized icons; not optimal for wildly varied sizes, but
    // simple and deterministic.
    function shelfPack(regions, maxWidth, padding) {
        padding = padding || 1;
        const items = regions.map((r, i) => ({
            i, name: r.name, w: r.width, h: r.height, draw: r.draw,
        }));
        items.sort((a, b) => b.h - a.h);

        let shelfX = 0, shelfY = 0, shelfH = 0;
        let totalW = 0, totalH = 0;
        const placed = [];
        for (const it of items) {
            if (shelfX + it.w > maxWidth && shelfX > 0) {
                shelfY += shelfH + padding;
                shelfX = 0;
                shelfH = 0;
            }
            placed.push({ ...it, x: shelfX, y: shelfY });
            shelfX += it.w + padding;
            if (it.h > shelfH) shelfH = it.h;
            if (shelfX > totalW) totalW = shelfX;
            if (shelfY + shelfH > totalH) totalH = shelfY + shelfH;
        }
        // Trim trailing padding.
        return { items: placed, width: totalW - padding, height: totalH };
    }

    function atlasLayout(spec) {
        const padding = spec.padding == null ? 1 : spec.padding;
        const maxWidth = spec.maxWidth || 256;
        return shelfPack(spec.regions, maxWidth, padding);
    }

    function renderAtlas(name) {
        const entry = REGISTRY[name];
        const spec = entry.spec;
        const layout = atlasLayout(spec);
        entry.layout = layout;
        const ctx = resetCanvas(sheetCanvas, layout.width, layout.height,
                                spec.bg, spec.pixel);
        for (const it of layout.items) {
            ctx.save();
            ctx.translate(it.x, it.y);
            try { it.draw(ctx, it.w, it.h, it.name); }
            catch (e) { console.log(`atlas region ${it.name} threw:`, e.message); }
            ctx.restore();
        }
    }

    // Step a procedural animation through virtual time and tile each frame
    // into a regular spritesheet. Same output shape as renderSheet so the
    // resulting PNG is interchangeable.
    function renderAnimated(name) {
        const entry = REGISTRY[name];
        const spec = entry.spec;
        const layout = animatedSize(spec);
        // Cache layout for buildManifest / save() to reuse without redoing
        // the rounding (fps/duration are floats; rounding once is cheaper
        // and keeps frame count identical between render and manifest).
        entry.layout = layout;
        const fw = spec.frameWidth, fh = spec.frameHeight;
        const ctx = resetCanvas(sheetCanvas, layout.w, layout.h,
                                spec.bg, spec.pixel);
        const dt = 1 / spec.fps;
        const state = (typeof spec.init === 'function') ? (spec.init() || {}) : {};
        for (let i = 0; i < layout.n; i++) {
            const cx = (i % layout.cols) * fw;
            const cy = Math.floor(i / layout.cols) * fh;
            const t = i * dt;
            ctx.save();
            // Clip so particles / shapes that overshoot the frame don't bleed
            // into neighboring cells in the sheet. The cell is the sprite's
            // visible bounds at runtime anyway, so anything outside is dropped.
            ctx.beginPath();
            ctx.rect(cx, cy, fw, fh);
            ctx.clip();
            ctx.translate(cx, cy);
            try { spec.frame(ctx, fw, fh, t, dt, state); }
            catch (e) { console.log(`frame ${i} threw:`, e.message); }
            ctx.restore();
        }
    }

    // ---- defineScene support --------------------------------------------
    //
    // Hidden canvas hosting a SceneGraph for sheet capture. display:none keeps
    // the engine's per-tick scene compositor from drawing it on screen; sizing
    // and rendering go through scene.captureFrame(w, h), which drives a
    // synchronous render and reads back the tonemap FBO. That works in both
    // windowed and headless modes — no flush() dependency.
    let captureCanvas = null;
    let captureScene  = null;

    function ensureCaptureCanvas() {
        if (!captureCanvas) {
            captureCanvas = document.createElement('canvas');
            captureCanvas.id = '__artstation_capture';
            captureCanvas.style.display = 'none';
            document.body.appendChild(captureCanvas);
            captureScene = captureCanvas.getContext('scene');
        }
        return captureScene;
    }

    // Wipe every node in the scene graph so a re-render with a different
    // asset (or a re-render of the same asset after an edit) doesn't carry
    // over stale meshes/lights from the previous build.
    function clearScene(scene) {
        if (!scene || !scene.root) return;
        const kids = scene.root.children.slice();
        for (const c of kids) {
            try { scene.destroyNode(c); } catch (e) {}
        }
    }

    function sceneSize(spec) {
        const n = Math.max(1, Math.round(spec.fps * spec.duration));
        const cols = spec.cols || Math.min(n, 8);
        const rows = Math.ceil(n / cols);
        return { w: spec.frameWidth * cols, h: spec.frameHeight * rows, cols, rows, n };
    }

    function renderScene(name) {
        const entry = REGISTRY[name];
        const spec = entry.spec;
        const layout = sceneSize(spec);
        entry.layout = layout;

        const fw = spec.frameWidth, fh = spec.frameHeight;
        const sheetCtx = resetCanvas(sheetCanvas, layout.w, layout.h, spec.bg, spec.pixel);

        const scene = ensureCaptureCanvas();
        clearScene(scene);

        const refs = spec.build(scene) || {};

        const dt = 1 / spec.fps;
        for (let i = 0; i < layout.n; i++) {
            const cx = (i % layout.cols) * fw;
            const cy = Math.floor(i / layout.cols) * fh;
            const t = i * dt;

            try { spec.frame(scene, t, dt, refs, i); }
            catch (e) { console.log(`scene frame ${i} threw:`, e.message); }

            const img = scene.captureFrame(fw, fh);
            if (img && img.width === fw && img.height === fh) {
                sheetCtx.putImageData(img, cx, cy);
            } else {
                console.log(`scene frame ${i}: captureFrame unavailable or size mismatch`);
            }
        }
    }

    // Expand the documented `frames: 'all'` sentinel into a concrete index
    // array so live preview / save paths can treat `frames` as an array
    // unconditionally. Mutates a shallow copy of `anims`; original spec
    // animations stay readable for re-renders after edits.
    function expandFrameSentinels(anims, frameCount) {
        if (!anims) return anims;
        const out = {};
        for (const [k, a] of Object.entries(anims)) {
            if (a && a.frames === 'all') {
                const all = [];
                for (let i = 0; i < frameCount; i++) all.push(i);
                out[k] = { ...a, frames: all };
            } else {
                out[k] = a;
            }
        }
        return out;
    }

    function buildManifest(name, entry) {
        const spec = entry.spec;
        switch (entry.kind) {
            case 'sheet': return {
                kind: 'sheet', src: `${name}.png`,
                frameWidth: spec.frameWidth, frameHeight: spec.frameHeight,
                cols: spec.cols, rows: spec.rows,
                frameCount: spec.frames.length,
                animations: expandFrameSentinels(spec.animations, spec.frames.length) || {},
            };
            case 'tileset': return {
                kind: 'tileset', src: `${name}.png`,
                tileSize: spec.tileSize, cols: spec.cols,
                tileCount: spec.tiles.length,
            };
            case 'image': return {
                kind: 'image', src: `${name}.png`,
                width: spec.width, height: spec.height,
            };
            case 'nineslice': return {
                kind: 'nineslice', src: `${name}.png`,
                width: spec.width, height: spec.height,
                slice: spec.slice,
            };
            case 'scene': {
                const lay = entry.layout || sceneSize(spec);
                const allFrames = [];
                for (let i = 0; i < lay.n; i++) allFrames.push(i);
                const anims = expandFrameSentinels(spec.animations, lay.n) || {
                    play: { frames: allFrames, fps: spec.fps, loop: false }
                };
                return {
                    kind: 'sheet', src: `${name}.png`,
                    frameWidth: spec.frameWidth, frameHeight: spec.frameHeight,
                    cols: lay.cols, rows: lay.rows,
                    frameCount: lay.n,
                    animations: anims,
                };
            }
            case 'animated': {
                const lay = entry.layout || animatedSize(spec);
                // Default to a single 'play' animation covering all frames.
                // User can override via spec.animations (with `frames: 'all'`
                // as a shorthand for the full range). Manifest output
                // mirrors defineSheet so consumers (scene.createSprite)
                // treat it as an ordinary sprite sheet.
                const allFrames = [];
                for (let i = 0; i < lay.n; i++) allFrames.push(i);
                const anims = expandFrameSentinels(spec.animations, lay.n) || {
                    play: { frames: allFrames, fps: spec.fps, loop: false }
                };
                return {
                    kind: 'sheet', src: `${name}.png`,
                    frameWidth: spec.frameWidth, frameHeight: spec.frameHeight,
                    cols: lay.cols, rows: lay.rows,
                    frameCount: lay.n,
                    animations: anims,
                };
            }
            case 'atlas': {
                const lay = entry.layout || atlasLayout(spec);
                const regions = {};
                for (const it of lay.items) {
                    regions[it.name] = { x: it.x, y: it.y, w: it.w, h: it.h };
                }
                return {
                    kind: 'atlas', src: `${name}.png`,
                    width: lay.width, height: lay.height,
                    regions,
                };
            }
        }
        return { kind: entry.kind, src: `${name}.png` };
    }

    function configurePixel(ctx) {
        // Pixel-perfect: integer coords, no smoothing.
        ctx.imageSmoothingEnabled = false;
    }

    // Smooth-mode: anti-aliased shapes, float coords.
    function configureSmooth(ctx) {
        ctx.imageSmoothingEnabled = true;
    }

    // Per-canvas display caps for resetCanvas. The buffer stays at full
    // resolution; only the CSS display dims get clamped here. Stage is
    // user-fixed at 320×240 in style.css so we leave it untouched.
    function displayCap(canvas) {
        if (canvas.id === 'sheet') return { w: 520, h: 720 };
        return { w: Infinity, h: Infinity };
    }

    // Resize a canvas to the target asset size and clear to bg. The buffer
    // (canvas.width / height) is set at full resolution so screenshot
    // exports stay pixel-perfect; the CSS display size is clamped to a
    // per-canvas max while preserving aspect ratio, so a 1024×768 sheet
    // doesn't blow out the windowed grid column.
    function resetCanvas(canvas, w, h, bg, pixel) {
        canvas.width  = w;
        canvas.height = h;
        const cap = displayCap(canvas);
        const scale = Math.min(1, cap.w / w, cap.h / h);
        canvas.style.width  = Math.round(w * scale) + 'px';
        canvas.style.height = Math.round(h * scale) + 'px';
        const ctx = canvas.getContext('2d');
        // Queue a full-surface clear regardless of dimensions — protects
        // against stale pixels surviving from a previous asset's draws when
        // the canvas pipeline hasn't yet caught up to the new size.
        if (typeof ctx.reset === 'function') ctx.reset();
        if (pixel === false) configureSmooth(ctx);
        else                 configurePixel(ctx);
        ctx.clearRect(0, 0, w, h);
        if (bg && bg !== 'transparent') {
            ctx.fillStyle = bg;
            ctx.fillRect(0, 0, w, h);
        }
        return ctx;
    }

    function renderSheet(name) {
        const entry = REGISTRY[name];
        const spec = entry.spec;
        const { w, h } = sheetSize(spec);
        const ctx = resetCanvas(sheetCanvas, w, h, spec.bg, spec.pixel);

        const fw = spec.frameWidth, fh = spec.frameHeight;
        for (let i = 0; i < spec.frames.length; i++) {
            const fn = spec.frames[i];
            if (!fn) continue;
            const cx = (i % spec.cols) * fw;
            const cy = Math.floor(i / spec.cols) * fh;
            ctx.save();
            // Clip so a frame can't bleed into its neighbor.
            ctx.beginPath();
            ctx.rect(cx, cy, fw, fh);
            ctx.clip();
            ctx.translate(cx, cy);
            try {
                fn(ctx, fw, fh, i);
            } catch (e) {
                console.log(`frame ${i} threw:`, e.message);
            }
            ctx.restore();
        }
    }

    function renderTileset(name) {
        const entry = REGISTRY[name];
        const spec = entry.spec;
        const { w, h } = tilesetSize(spec);
        const ctx = resetCanvas(sheetCanvas, w, h, spec.bg, spec.pixel);

        const ts = spec.tileSize;
        for (let i = 0; i < spec.tiles.length; i++) {
            const fn = spec.tiles[i];
            if (!fn) continue;
            const cx = (i % spec.cols) * ts;
            const cy = Math.floor(i / spec.cols) * ts;
            ctx.save();
            ctx.beginPath();
            ctx.rect(cx, cy, ts, ts);
            ctx.clip();
            ctx.translate(cx, cy);
            try {
                fn(ctx, ts, i);
            } catch (e) {
                console.log(`tile ${i} threw:`, e.message);
            }
            ctx.restore();
        }
    }

    function renderImage(name) {
        const spec = REGISTRY[name].spec;
        const ctx = resetCanvas(sheetCanvas, spec.width, spec.height, spec.bg, spec.pixel);
        try { spec.draw(ctx, spec.width, spec.height); }
        catch (e) { console.log('draw threw:', e.message); }
    }

    // Same render path as image; slice metadata stays in the manifest.
    function renderNineSlice(name) { renderImage(name); }

    function render(name) {
        name = name || CURRENT;
        if (!name) throw new Error('nothing loaded — call load("name") first');
        CURRENT = name;
        const entry = REGISTRY[name];
        switch (entry.kind) {
            case 'sheet':     renderSheet(name); break;
            case 'tileset':   renderTileset(name); break;
            case 'image':     renderImage(name); break;
            case 'nineslice': renderNineSlice(name); break;
            case 'atlas':     renderAtlas(name); break;
            case 'animated':  renderAnimated(name); break;
            case 'scene':     renderScene(name); break;
            default: throw new Error('unknown kind: ' + entry.kind);
        }
        updateInfo(name);
    }

    // ---------- Save (PNG of sheet canvas) ------------------------------

    // Saves the sheet canvas to output/<name>.png and writes a sidecar
    // <name>.json with the manifest (frame size, animations, etc) so
    // game code can load both with one fetch.
    //
    // Uses screenshotCanvas (engine-level binding, available in both
    // windowed and headless) which snapshots the canvas's Skia surface
    // directly and preserves alpha — bypassing the framebuffer composite
    // path that would flatten transparent pixels to opaque black.
    function save(name) {
        name = name || CURRENT;
        if (!name) throw new Error('nothing to save');
        if (typeof screenshotCanvas !== 'function') {
            throw new Error('save() requires the screenshotCanvas global');
        }
        const entry = REGISTRY[name];
        const outDir = 'apps/artstation/output';
        const pngPath = `${outDir}/${name}.png`;
        const jsonPath = `${outDir}/${name}.json`;

        // Direct canvas-surface snapshot — preserves alpha (the framebuffer
        // composite path used by screenshot() flattens transparency).
        screenshotCanvas(pngPath, '#sheet');

        const meta = buildManifest(name, entry);

        // brokit fs is exposed as `bro.fs` and as require('fs'). Try both.
        try {
            const fs = (typeof require === 'function') ? require('fs') : null;
            if (fs && fs.writeFileSync) {
                fs.writeFileSync(jsonPath, JSON.stringify(meta, null, 2));
            }
        } catch (e) {
            console.log('manifest write failed:', e.message);
        }

        status.textContent = `saved ${pngPath}`;
        return { png: pngPath, json: jsonPath, meta };
    }

    // ---------- Save video / GIF (procedural animation → file) ----------

    // Drive `addFrame(ctx, sheetCanvas)` once per logical frame for an
    // animated or sheet asset. Resizes the sheet canvas to frame size and
    // either runs the procedural step (animated) or replays the picked
    // animation's hand-drawn frame functions (sheet). Restores the full
    // spritesheet render at the end so the UI doesn't end up stuck on the
    // last single frame.
    //
    // Returns { fw, fh, framesEmitted, fps } describing what got encoded;
    // callers attach this onto whatever encoder-specific result they return.
    function driveAnimationFrames(name, opts, addFrame) {
        const entry = REGISTRY[name];
        const spec = entry.spec;
        opts = opts || {};

        let fw, fh, fps, frames;

        if (entry.kind === 'scene') {
            // Encode each scene frame straight from the offscreen capture
            // canvas: re-run build/frame, flush, putImageData onto a sheet
            // canvas now sized to the cell dimensions, addFrame. The scene
            // capture path mirrors renderScene() — we just deposit every
            // frame at (0,0) instead of tiling them across a sheet.
            fw = spec.frameWidth; fh = spec.frameHeight;
            fps = opts.fps || spec.fps;
            const dt = 1 / spec.fps;
            const total = Math.max(1, Math.round(spec.fps * spec.duration));
            const ctx = resetCanvas(sheetCanvas, fw, fh, spec.bg, spec.pixel);
            const scene = ensureCaptureCanvas();
            clearScene(scene);
            const refs = spec.build(scene) || {};
            for (let i = 0; i < total; i++) {
                resetCanvas(sheetCanvas, fw, fh, spec.bg, spec.pixel);
                try { spec.frame(scene, i * dt, dt, refs, i); }
                catch (e) { console.log(`scene frame ${i} threw:`, e.message); }
                const img = scene.captureFrame(fw, fh);
                if (img) ctx.putImageData(img, 0, 0);
                addFrame(ctx, sheetCanvas, i);
            }
            frames = total;
            return { fw, fh, fps, framesEmitted: frames };
        }

        if (entry.kind === 'animated') {
            fw = spec.frameWidth; fh = spec.frameHeight;
            fps = opts.fps || spec.fps;
            const dt = 1 / spec.fps;
            const total = animatedFrameCount(spec);
            const ctx = resetCanvas(sheetCanvas, fw, fh, spec.bg, spec.pixel);
            const state = (typeof spec.init === 'function') ? (spec.init() || {}) : {};
            for (let i = 0; i < total; i++) {
                resetCanvas(sheetCanvas, fw, fh, spec.bg, spec.pixel);
                ctx.save();
                try { spec.frame(ctx, fw, fh, i * dt, dt, state); }
                catch (e) { console.log(`frame ${i} threw:`, e.message); }
                ctx.restore();
                if (typeof flush === 'function') flush();
                addFrame(ctx, sheetCanvas, i);
            }
            frames = total;
        } else if (entry.kind === 'sheet') {
            // Pick an animation: explicit opts.anim, then 'idle'/'play'/'loop',
            // then the first one defined. Sheets without animations encode
            // the whole frames[] array in declaration order.
            fw = spec.frameWidth; fh = spec.frameHeight;
            const anims = spec.animations || {};
            const animKeys = Object.keys(anims);
            let frameIndices;
            if (opts.anim && anims[opts.anim]) {
                frameIndices = anims[opts.anim].frames.slice();
                fps = opts.fps || anims[opts.anim].fps || 8;
            } else if (animKeys.length > 0) {
                const pick = ['idle','play','loop'].find(k => anims[k]) || animKeys[0];
                frameIndices = anims[pick].frames.slice();
                fps = opts.fps || anims[pick].fps || 8;
            } else {
                frameIndices = spec.frames.map((_, i) => i);
                fps = opts.fps || 8;
            }
            const ctx = resetCanvas(sheetCanvas, fw, fh, spec.bg, spec.pixel);
            for (let i = 0; i < frameIndices.length; i++) {
                const idx = frameIndices[i];
                const fn = spec.frames[idx];
                resetCanvas(sheetCanvas, fw, fh, spec.bg, spec.pixel);
                if (fn) {
                    ctx.save();
                    try { fn(ctx, fw, fh, idx); }
                    catch (e) { console.log(`sheet frame ${idx} threw:`, e.message); }
                    ctx.restore();
                }
                if (typeof flush === 'function') flush();
                addFrame(ctx, sheetCanvas, i);
            }
            frames = frameIndices.length;
        } else {
            throw new Error(`save video/gif requires animated, sheet, or scene asset, not ${entry.kind}`);
        }

        return { fw, fh, fps, framesEmitted: frames };
    }

    // Pick a default output path for the given asset + format. Sheets get
    // an animation suffix (when one is chosen) so multiple animations can
    // coexist as separate files; animated assets just take the asset name.
    function defaultExportPath(name, opts, ext) {
        opts = opts || {};
        const entry = REGISTRY[name];
        const outDir = 'apps/artstation/output';
        if (opts.path) return opts.path;
        if (entry.kind === 'sheet') {
            const anims = entry.spec.animations || {};
            const animKeys = Object.keys(anims);
            let pick = opts.anim;
            if (!pick && animKeys.length > 0) {
                pick = ['idle','play','loop'].find(k => anims[k]) || animKeys[0];
            }
            return pick ? `${outDir}/${name}_${pick}.${ext}` : `${outDir}/${name}.${ext}`;
        }
        return `${outDir}/${name}.${ext}`;
    }

    // Merge windowed transport state (selected animation + speed multiplier)
    // into save opts so an export plays back identically to the on-screen
    // preview. Explicit caller opts win (so headless tests stay deterministic).
    function applyPlaybackOpts(name, opts) {
        const entry = REGISTRY[name];
        const out = Object.assign({}, opts || {});
        if (!out.anim && playback.anim) out.anim = playback.anim;
        if (out.fps == null) {
            const spec = entry.spec;
            let baseFps;
            if (entry.kind === 'animated' || entry.kind === 'scene') {
                baseFps = spec.fps;
            } else {
                const anims = spec.animations || {};
                const pick = (out.anim && anims[out.anim])
                    ? out.anim
                    : (['idle','play','loop'].find(k => anims[k]) || Object.keys(anims)[0]);
                baseFps = (pick && anims[pick] && anims[pick].fps) || 8;
            }
            const speed = playback.speed || 1;
            out.fps = baseFps * speed;
        }
        return out;
    }

    // saveVideo(name?, opts?) — encode an `animated` or `sheet` asset to a
    // VP9/WebM file. opts:
    //   path        — override output path
    //   fps         — encoder fps (defaults: animated→spec.fps, sheet→anim.fps)
    //   bitrateKbps — VBR target (default auto in encoder)
    //   quality     — 'realtime' | 'good' | 'best' (default 'good')
    //   anim        — for sheet kind: animation name to encode (default: first)
    //
    // Frame size must be even (VP9 4:2:0 chroma) — odd sizes throw.
    function saveVideo(name, opts) {
        name = name || CURRENT;
        if (!name) throw new Error('nothing to saveVideo');
        if (typeof VideoEncoder !== 'function') {
            throw new Error('saveVideo() requires the VideoEncoder global');
        }
        const entry = REGISTRY[name];
        if (!entry) throw new Error(`asset not loaded: ${name}`);
        if (entry.kind !== 'animated' && entry.kind !== 'sheet' && entry.kind !== 'scene') {
            throw new Error('saveVideo() only supports animated, sheet, or scene assets');
        }
        const spec = entry.spec;
        if ((spec.frameWidth & 1) || (spec.frameHeight & 1)) {
            throw new Error(
                `saveVideo: frame size ${spec.frameWidth}x${spec.frameHeight} ` +
                `must be even (VP9 4:2:0 chroma)`);
        }

        opts = applyPlaybackOpts(name, opts);
        const path    = defaultExportPath(name, opts, 'webm');
        const quality = opts.quality || 'good';

        let enc = null;
        let result = null;
        try {
            const stats = driveAnimationFrames(name, opts, (ctx, canvas, i) => {
                if (!enc) {
                    enc = new VideoEncoder({
                        path,
                        width: canvas.width, height: canvas.height,
                        fps: opts.fps || (entry.kind === 'animated' ? spec.fps : 8),
                        quality,
                        bitrateKbps: opts.bitrateKbps || 0,
                    });
                }
                enc.addCanvasFrame(canvas);
            });
            if (enc && !enc.finish()) {
                throw new Error('encoder finish failed: ' + enc.lastError);
            }
            result = {
                path, frames: enc ? enc.framesWritten : 0,
                fps: stats.fps, width: stats.fw, height: stats.fh,
            };
        } finally {
            // Re-render rebuilds the capture SceneGraph (clearScene + build),
            // which invalidates the refs the live preview captured. Restart
            // it so the stage keeps animating after save returns.
            try { render(name); } catch (e) {}
            try { startLivePreview(name); } catch (e) {}
        }

        status.textContent = `saved ${path} (${result.frames} frames)`;
        return result;
    }

    // saveGif(name?, opts?) — same shape as saveVideo, writes an animated
    // GIF89a instead. opts:
    //   path        — override output path
    //   fps         — defaults same as saveVideo
    //   paletteBits — 1..8, defaults 8 (256 colors per frame)
    //   loopCount   — 0 = infinite (default), 1 = play once, N = repeat N
    //   anim        — for sheet kind: animation name to encode
    function saveGif(name, opts) {
        name = name || CURRENT;
        if (!name) throw new Error('nothing to saveGif');
        if (typeof GifEncoder !== 'function') {
            throw new Error('saveGif() requires the GifEncoder global');
        }
        const entry = REGISTRY[name];
        if (!entry) throw new Error(`asset not loaded: ${name}`);
        if (entry.kind !== 'animated' && entry.kind !== 'sheet' && entry.kind !== 'scene') {
            throw new Error('saveGif() only supports animated, sheet, or scene assets');
        }

        opts = applyPlaybackOpts(name, opts);
        const path = defaultExportPath(name, opts, 'gif');
        const spec = entry.spec;

        let enc = null;
        let result = null;
        try {
            const stats = driveAnimationFrames(name, opts, (ctx, canvas, i) => {
                if (!enc) {
                    enc = new GifEncoder({
                        path,
                        width: canvas.width, height: canvas.height,
                        fps: opts.fps || (entry.kind === 'animated' ? spec.fps : 8),
                        paletteBits: opts.paletteBits || 8,
                        loopCount: (opts.loopCount == null) ? 0 : opts.loopCount,
                    });
                }
                enc.addCanvasFrame(canvas);
            });
            if (enc && !enc.finish()) {
                throw new Error('encoder finish failed: ' + enc.lastError);
            }
            result = {
                path, frames: enc ? enc.framesWritten : 0,
                fps: stats.fps, width: stats.fw, height: stats.fh,
            };
        } finally {
            // Re-render rebuilds the capture SceneGraph (clearScene + build),
            // which invalidates the refs the live preview captured. Restart
            // it so the stage keeps animating after save returns.
            try { render(name); } catch (e) {}
            try { startLivePreview(name); } catch (e) {}
        }

        status.textContent = `saved ${path} (${result.frames} frames)`;
        return result;
    }

    // ---------- Preview (animate the produced sheet via scene API) ------

    let stageScene = null;
    let stageSprite = null;

    function preview(animName) {
        if (!CURRENT) throw new Error('nothing loaded');
        const entry = REGISTRY[CURRENT];
        if (entry.kind !== 'sheet' && entry.kind !== 'animated') {
            throw new Error('preview() only works for sheets / animated');
        }
        if (!stageScene) {
            stageScene = stageCanvas.getContext('scene');
        }
        if (stageSprite) { stageSprite.destroy(); stageSprite = null; }

        const spec = entry.spec;
        // Animated assets compute cols/rows from fps*duration; sheet has them
        // explicitly. Pull from the manifest so both kinds use the same path.
        const meta = buildManifest(CURRENT, entry);
        stageSprite = stageScene.createSprite({
            src: `output/${CURRENT}.png`,
            sheet: {
                frameWidth: meta.frameWidth,
                frameHeight: meta.frameHeight,
                columns: meta.cols,
                rows: meta.rows,
            },
            animations: meta.animations || {},
            x: stageCanvas.width / 2,
            y: stageCanvas.height / 2,
            width: meta.frameWidth * 4,
            height: meta.frameHeight * 4,
        });
        const auto = (entry.kind === 'animated' && !animName && meta.animations.play)
            ? 'play' : animName;
        if (auto && meta.animations && meta.animations[auto]) {
            stageSprite.play(auto);
        }
        status.textContent = `previewing ${CURRENT}` + (auto ? `:${auto}` : '');
    }

    // ---------- Tilemap preview (lay out the tileset into a small map) --

    function previewMap(layoutFn) {
        if (!CURRENT) throw new Error('nothing loaded');
        const entry = REGISTRY[CURRENT];
        if (entry.kind !== 'tileset') {
            throw new Error('previewMap() only works for tilesets');
        }
        if (!stageScene) stageScene = stageCanvas.getContext('scene');
        if (stageSprite) { stageSprite.destroy(); stageSprite = null; }

        const ts = entry.spec.tileSize;
        // 16x12 cells, scaled 2x on stage.
        const cols = 16, rows = 12;
        const data = new Uint16Array(cols * rows);
        if (typeof layoutFn === 'function') {
            for (let r = 0; r < rows; r++)
                for (let c = 0; c < cols; c++)
                    data[r * cols + c] = layoutFn(c, r) | 0;
        } else {
            // Default: cycle through every defined tile.
            for (let i = 0; i < data.length; i++) {
                data[i] = (i % (entry.spec.tiles.length - 1)) + 1;
            }
        }
        stageSprite = stageScene.createTilemap({
            tileWidth: ts * 2,
            tileHeight: ts * 2,
            columns: cols, rows,
            tileset: { src: `output/${CURRENT}.png`, tileWidth: ts, tileHeight: ts, columns: entry.spec.cols },
            data,
        });
        status.textContent = `tilemap preview: ${CURRENT}`;
    }

    // ---------- Info pane ------------------------------------------------

    function updateInfo(name) {
        const entry = REGISTRY[name];
        if (!entry) { info.textContent = 'no asset'; return; }
        const lines = [];
        lines.push(`name:   ${name}`);
        lines.push(`kind:   ${entry.kind}`);
        if (entry.kind === 'sheet') {
            const s = entry.spec;
            lines.push(`frame:  ${s.frameWidth} x ${s.frameHeight}`);
            lines.push(`grid:   ${s.cols} x ${s.rows}`);
            lines.push(`frames: ${s.frames.length}`);
            const anims = Object.keys(s.animations || {});
            if (anims.length) {
                lines.push(`anims:`);
                for (const a of anims) {
                    const an = s.animations[a];
                    lines.push(`  ${a}: ${an.frames.length}f @ ${an.fps}fps${an.loop?' loop':''}`);
                }
            }
        } else if (entry.kind === 'tileset') {
            const s = entry.spec;
            lines.push(`tile:   ${s.tileSize} x ${s.tileSize}`);
            lines.push(`tiles:  ${s.tiles.length} (cols=${s.cols})`);
        } else if (entry.kind === 'image' || entry.kind === 'nineslice') {
            lines.push(`size:   ${entry.spec.width} x ${entry.spec.height}`);
        } else if (entry.kind === 'atlas') {
            const lay = entry.layout || atlasLayout(entry.spec);
            lines.push(`region: ${entry.spec.regions.length}`);
            lines.push(`pack:   ${lay.width} x ${lay.height}`);
        } else if (entry.kind === 'animated') {
            const s = entry.spec;
            const lay = entry.layout || animatedSize(s);
            lines.push(`frame:  ${s.frameWidth} x ${s.frameHeight}`);
            lines.push(`grid:   ${lay.cols} x ${lay.rows}`);
            lines.push(`frames: ${lay.n} (${s.duration}s @ ${s.fps}fps)`);
        } else if (entry.kind === 'scene') {
            const s = entry.spec;
            const lay = entry.layout || sceneSize(s);
            lines.push(`frame:  ${s.frameWidth} x ${s.frameHeight} (3D)`);
            lines.push(`grid:   ${lay.cols} x ${lay.rows}`);
            lines.push(`frames: ${lay.n} (${s.duration}s @ ${s.fps}fps)`);
        }
        let sz;
        if      (entry.kind === 'sheet')    sz = sheetSize(entry.spec);
        else if (entry.kind === 'tileset')  sz = tilesetSize(entry.spec);
        else if (entry.kind === 'atlas')    { const l = entry.layout || atlasLayout(entry.spec); sz = { w: l.width, h: l.height }; }
        else if (entry.kind === 'animated') { const l = entry.layout || animatedSize(entry.spec); sz = { w: l.w, h: l.h }; }
        else if (entry.kind === 'scene')    { const l = entry.layout || sceneSize(entry.spec); sz = { w: l.w, h: l.h }; }
        else                                sz = { w: entry.spec.width, h: entry.spec.height };
        lines.push(`png:    ${sz.w} x ${sz.h} px`);
        if (entry.kind === 'nineslice') {
            const s = entry.spec.slice;
            lines.push(`slice:  L${s.left} R${s.right} T${s.top} B${s.bottom}`);
        }
        info.textContent = lines.join('\n');
    }

    // ---------- Windowed: asset picker + live preview + hot reload ------
    //
    // Headless drives the app via load/render/save explicitly. Windowed mode
    // is the artist's view: discover every asset, list them as buttons, and
    // when one is clicked render it + animate it live. fs.watch the assets
    // directory so saving an asset module re-renders + restarts the loop
    // without an app restart.
    //
    // Live preview bypasses the headless save→PNG→createSprite roundtrip
    // because save() needs screenshotCanvas (headless-only). Instead we
    // blit cells straight from the sheet canvas onto the stage canvas at
    // the correct fps. The PNG export path is unchanged.

    const picker       = document.getElementById('picker');
    const watchStatus  = document.getElementById('watch-status');
    let livePreviewRAF = null;
    let liveStartTime  = 0;
    let liveAnimSpec   = null;  // { frames: [i,...], fps, loop } for sheet
    let liveState      = null;  // mutable state for animated kind
    let liveLastTickMs = 0;     // wall-clock of last animated tick
    let liveAccumS     = 0;     // accumulated seconds toward next frame
    let liveFrameIdx   = 0;     // current frame for animated
    let watcher        = null;
    let watchTimers    = {};    // name -> debounce timer id

    // Transport state — drives BOTH the live preview and the save path.
    // anim: name of the picked animation (sheet kind only; null/'play' for
    //   animated). saveVideo/saveGif read playback.anim before falling back
    //   to their own anim picker.
    // speed: playback rate multiplier. 1 = native fps. Live preview multiplies
    //   the rate; save multiplies the encoder fps so the exported file plays
    //   at the same wall-clock speed as the on-screen preview.
    const playback = { paused: false, speed: 1, anim: null };

    function isWindowed() {
        // advanceTime() is headless-only (screenshotCanvas is in both modes).
        return typeof advanceTime === 'undefined';
    }

    function discoverAssets() {
        const fs = require('fs');
        const names = [];
        try {
            const entries = fs.readdirSync('assets');
            for (const e of entries) {
                if (typeof e === 'string' && e.endsWith('.js')) {
                    names.push(e.slice(0, -3));
                }
            }
        } catch (err) {
            console.log('discoverAssets failed:', err.message);
        }
        names.sort();
        return names;
    }

    function rebuildPicker() {
        if (!picker) return;
        picker.textContent = '';
        const names = discoverAssets();
        for (const name of names) {
            const btn = document.createElement('button');
            btn.className = 'asset-btn';
            btn.dataset.name = name;
            const kind = REGISTRY[name] ? REGISTRY[name].kind : '?';
            btn.textContent = name;
            const kSpan = document.createElement('span');
            kSpan.className = 'kind';
            kSpan.textContent = kind;
            btn.appendChild(kSpan);
            if (name === CURRENT) btn.classList.add('selected');
            btn.addEventListener('click', () => selectAsset(name));
            picker.appendChild(btn);
        }
    }

    function refreshPickerSelection() {
        if (!picker) return;
        for (const btn of picker.querySelectorAll('.asset-btn')) {
            btn.classList.toggle('selected', btn.dataset.name === CURRENT);
            const kSpan = btn.querySelector('.kind');
            if (kSpan && REGISTRY[btn.dataset.name]) {
                kSpan.textContent = REGISTRY[btn.dataset.name].kind;
            }
        }
    }

    // Stop any running live preview loop.
    function stopLivePreview() {
        if (livePreviewRAF) {
            cancelAnimationFrame(livePreviewRAF);
            livePreviewRAF = null;
        }
        liveAnimSpec = null;
        liveState    = null;
        // Revert to 2D stage; scene-kind preview will re-show #stage-3d.
        showStage3D(false);
    }

    // Pick the first animation from a sheet/animated manifest.
    function defaultAnimation(meta) {
        const anims = meta.animations || {};
        const keys = Object.keys(anims);
        if (keys.length === 0) {
            const all = [];
            for (let i = 0; i < meta.frameCount; i++) all.push(i);
            return { frames: all, fps: 8, loop: true };
        }
        for (const pref of ['idle', 'play', 'loop']) {
            if (anims[pref]) return anims[pref];
        }
        return anims[keys[0]];
    }

    // Pick the largest integer scale that fits a (w, h) into the stage with
    // a small margin. Used by every live-preview path.
    function fitScale(w, h, margin) {
        margin = margin == null ? 32 : margin;
        const maxW = stageCanvas.width  - margin;
        const maxH = stageCanvas.height - margin;
        return Math.max(1, Math.min(Math.floor(maxW / w),
                                    Math.floor(maxH / h)));
    }

    // Clear stage + return its 2D context configured for pixel art.
    function resetStage(bg) {
        const ctx = stageCanvas.getContext('2d');
        ctx.imageSmoothingEnabled = false;
        ctx.fillStyle = bg || '#111';
        ctx.fillRect(0, 0, stageCanvas.width, stageCanvas.height);
        return ctx;
    }


    // Run drawFn(ctx, w, h) inside a clipped, scaled, centered region of
    // the stage. Coordinates inside drawFn are local to (0,0)..(w,h).
    function withCenteredCell(ctx, w, h, drawFn) {
        const scale = fitScale(w, h);
        const dw = w * scale, dh = h * scale;
        const dx = Math.floor((stageCanvas.width  - dw) / 2);
        const dy = Math.floor((stageCanvas.height - dh) / 2);
        ctx.save();
        ctx.beginPath();
        ctx.rect(dx, dy, dw, dh);
        ctx.clip();
        ctx.translate(dx, dy);
        ctx.scale(scale, scale);
        try { drawFn(ctx); } catch (e) { console.log('live draw threw:', e.message); }
        ctx.restore();
    }

    // ── Per-kind live frame draws. All draw directly from the asset spec.
    //   We never read from the sheet canvas — cross-canvas drawImage isn't
    //   reliable through bro's GPU compositor and made the stage stay blank
    //   while the sheet flickered. Drawing from spec is also more honest:
    //   what you see on stage is exactly what your code produces, with no
    //   intermediate raster buffer to get out of sync.

    function drawSheetFrameLive(spec, idx) {
        const fn = spec.frames[idx];
        if (!fn) return;
        const ctx = resetStage();
        withCenteredCell(ctx, spec.frameWidth, spec.frameHeight, (c) => {
            fn(c, spec.frameWidth, spec.frameHeight, idx);
        });
    }

    function drawAnimatedFrameLive(spec, t, dt, state) {
        const ctx = resetStage();
        withCenteredCell(ctx, spec.frameWidth, spec.frameHeight, (c) => {
            spec.frame(c, spec.frameWidth, spec.frameHeight, t, dt, state);
        });
    }

    function drawTilesetLive(spec) {
        const ctx = resetStage();
        const ts = spec.tileSize;
        const scale = 2;
        const cellsX = Math.floor(stageCanvas.width  / (ts * scale));
        const cellsY = Math.floor(stageCanvas.height / (ts * scale));
        const defined = spec.tiles.length - 1; // index 0 reserved
        if (defined <= 0) return;
        for (let r = 0; r < cellsY; r++) {
            for (let c = 0; c < cellsX; c++) {
                const idx = ((r * cellsX + c) % defined) + 1;
                const fn = spec.tiles[idx];
                if (!fn) continue;
                ctx.save();
                ctx.beginPath();
                ctx.rect(c * ts * scale, r * ts * scale, ts * scale, ts * scale);
                ctx.clip();
                ctx.translate(c * ts * scale, r * ts * scale);
                ctx.scale(scale, scale);
                try { fn(ctx, ts, idx); } catch (e) {}
                ctx.restore();
            }
        }
    }

    function drawImageLive(spec) {
        const ctx = resetStage();
        withCenteredCell(ctx, spec.width, spec.height, (c) => {
            spec.draw(c, spec.width, spec.height);
        });
    }

    function drawAtlasLive(name) {
        const entry = REGISTRY[name];
        const layout = entry.layout || atlasLayout(entry.spec);
        const ctx = resetStage();
        withCenteredCell(ctx, layout.width, layout.height, (c) => {
            for (const it of layout.items) {
                c.save();
                c.beginPath();
                c.rect(it.x, it.y, it.w, it.h);
                c.clip();
                c.translate(it.x, it.y);
                try { it.draw(c, it.w, it.h, it.name); } catch (e) {}
                c.restore();
            }
        });
    }

    function startLivePreview(name) {
        stopLivePreview();
        const entry = REGISTRY[name];
        if (!entry) return;

        const spec = entry.spec;

        if (entry.kind === 'tileset')   { drawTilesetLive(spec);  return; }
        if (entry.kind === 'image' ||
            entry.kind === 'nineslice') { drawImageLive(spec);    return; }
        if (entry.kind === 'atlas')     { drawAtlasLive(name);    return; }

        if (entry.kind === 'sheet') {
            const meta = buildManifest(name, entry);
            // Honor the picked animation if it exists in the manifest;
            // otherwise fall back to the default pick.
            const anims = meta.animations || {};
            liveAnimSpec  = (playback.anim && anims[playback.anim])
                ? anims[playback.anim]
                : defaultAnimation(meta);
            liveStartTime = performance.now();
            liveAccumS    = 0;        // logical seconds played, advances by realDt*speed
            liveLastTickMs = liveStartTime;
            const loop = (t) => {
                if (!liveAnimSpec || CURRENT !== name) return;
                let realDt = (t - liveLastTickMs) / 1000;
                if (!isFinite(realDt) || realDt < 0) realDt = 0;
                liveLastTickMs = t;
                if (!playback.paused) liveAccumS += realDt * playback.speed;
                const idx = Math.floor(liveAccumS * liveAnimSpec.fps);
                const len = liveAnimSpec.frames.length;
                const slot = liveAnimSpec.loop
                    ? (idx % len)
                    : Math.min(idx, len - 1);
                const frameIdx = liveAnimSpec.frames[slot];
                drawSheetFrameLive(spec, frameIdx);
                updateFrameCounter(slot, len);
                livePreviewRAF = requestAnimationFrame(loop);
            };
            livePreviewRAF = requestAnimationFrame(loop);
            return;
        }

        if (entry.kind === 'scene') {
            // Scene live preview renders to a visible scene canvas and lets
            // the engine composite it per tick — no captureFrame, no FBO
            // readback, no putImageData. The rAF loop just mutates the
            // graph via spec.frame(); cost per tick is whatever spec.frame
            // does plus engine compositing (which would happen anyway).
            // captureFrame stays in renderScene/saveVideo/saveGif where the
            // pixels actually need to land in JS-land.
            const meta = buildManifest(name, entry);
            const anims = meta.animations || {};
            liveAnimSpec  = (playback.anim && anims[playback.anim])
                ? anims[playback.anim]
                : defaultAnimation(meta);

            // Size the visible scene canvas to match the asset at the
            // largest integer scale that fits the stage area, then swap
            // visibility from the 2D stage to the 3D one.
            const fw = spec.frameWidth, fh = spec.frameHeight;
            const scale = fitScale(fw, fh);
            const bw = fw * scale, bh = fh * scale;
            if (stage3dCanvas.width  !== bw) stage3dCanvas.width  = bw;
            if (stage3dCanvas.height !== bh) stage3dCanvas.height = bh;
            stage3dCanvas.style.width  = bw + 'px';
            stage3dCanvas.style.height = bh + 'px';
            showStage3D(true);

            const liveScene = ensureStage3D();
            clearScene(liveScene);
            let liveRefs = {};
            try { liveRefs = spec.build(liveScene) || {}; }
            catch (e) { console.log('scene build threw:', e.message); }

            const dtLogical = 1 / spec.fps;
            liveStartTime  = performance.now();
            liveAccumS     = 0;
            liveLastTickMs = liveStartTime;
            const loop = (t) => {
                if (!liveAnimSpec || CURRENT !== name) return;
                let realDt = (t - liveLastTickMs) / 1000;
                if (!isFinite(realDt) || realDt < 0) realDt = 0;
                liveLastTickMs = t;
                if (!playback.paused) liveAccumS += realDt * playback.speed;
                const idx = Math.floor(liveAccumS * liveAnimSpec.fps);
                const len = liveAnimSpec.frames.length;
                const slot = liveAnimSpec.loop
                    ? (idx % len)
                    : Math.min(idx, len - 1);
                const frameIdx = liveAnimSpec.frames[slot];
                // Continuous t for smooth motion; idx still passed for any
                // frame fn that wants per-cell switching. dt stays at the
                // logical 1/fps so velocity*dt math matches the captured
                // sheet's per-cell step.
                try { spec.frame(liveScene, liveAccumS, dtLogical, liveRefs, frameIdx); }
                catch (e) { console.log(`scene frame ${frameIdx} threw:`, e.message); }
                updateFrameCounter(slot, len);
                livePreviewRAF = requestAnimationFrame(loop);
            };
            livePreviewRAF = requestAnimationFrame(loop);
            return;
        }

        if (entry.kind === 'animated') {
            // Animated kind: state evolves over time. We step state forward
            // by dt (1/fps) each animation frame; rAF runs faster than fps
            // so we accumulate real-time and step when enough has passed.
            const totalFrames = Math.max(1, Math.round(spec.fps * spec.duration));
            const dt = 1 / spec.fps;
            liveState      = (typeof spec.init === 'function') ? (spec.init() || {}) : {};
            liveFrameIdx   = 0;
            liveAccumS     = 0;
            liveLastTickMs = performance.now();
            liveAnimSpec   = { kind: 'animated' }; // sentinel so stop checks work

            const loop = (t) => {
                if (!liveAnimSpec || CURRENT !== name) return;
                // Clamp realDt so a paused/backgrounded window or a wonky
                // first timestamp can't cause the inner step loop to run
                // thousands of times. Cap at one logical animation cycle.
                let realDt = (t - liveLastTickMs) / 1000;
                if (!isFinite(realDt) || realDt < 0) realDt = dt;
                if (realDt > spec.duration) realDt = spec.duration;
                liveLastTickMs = t;
                if (!playback.paused) liveAccumS += realDt * playback.speed;
                let stepCount = 0;
                while (liveAccumS >= dt && stepCount < totalFrames * 2) {
                    liveAccumS -= dt;
                    liveFrameIdx++;
                    stepCount++;
                    if (liveFrameIdx >= totalFrames) {
                        // Loop: re-seed state so particles / springs / etc.
                        // restart from a clean slate.
                        liveState = (typeof spec.init === 'function') ? (spec.init() || {}) : {};
                        liveFrameIdx = 0;
                    }
                }
                drawAnimatedFrameLive(spec, liveFrameIdx * dt, dt, liveState);
                updateFrameCounter(liveFrameIdx, totalFrames);
                livePreviewRAF = requestAnimationFrame(loop);
            };
            livePreviewRAF = requestAnimationFrame(loop);
            return;
        }

        // Static kinds: no frame counter.
        updateFrameCounter(null, null);
    }

    // ---------- Transport (play/pause + speed + animation picker) -------

    const playToggle    = document.getElementById('play-toggle');
    const animSelect    = document.getElementById('anim-select');
    const speedButtons  = document.getElementById('speed-buttons');
    const frameCounter  = document.getElementById('frame-counter');

    function updateFrameCounter(idx, total) {
        if (!frameCounter) return;
        if (idx == null || total == null) { frameCounter.textContent = ''; return; }
        frameCounter.textContent = `frame ${idx + 1} / ${total}`;
    }

    function setPaused(paused) {
        playback.paused = paused;
        if (playToggle) playToggle.textContent = paused ? '▶' : '⏸';
        if (paused) liveLastTickMs = performance.now();
    }

    function setSpeed(s) {
        playback.speed = s;
        if (!speedButtons) return;
        for (const b of speedButtons.querySelectorAll('.speed-btn')) {
            b.classList.toggle('selected', parseFloat(b.dataset.speed) === s);
        }
    }

    // Populate the anim dropdown for the current asset. Sheet assets list
    // every animation; animated assets show the synthetic 'play' entry;
    // others disable the picker. Restarting the live preview is the caller's
    // job — refreshTransport just resyncs the controls to the asset.
    function refreshTransport() {
        if (!animSelect) return;
        const entry = CURRENT ? REGISTRY[CURRENT] : null;
        animSelect.textContent = '';
        if (!entry) {
            animSelect.disabled = true;
            updateFrameCounter(null, null);
            return;
        }
        let names = [];
        if (entry.kind === 'sheet' || entry.kind === 'scene') {
            names = Object.keys(entry.spec.animations || {});
            if (names.length === 0) names = ['(all frames)'];
        } else if (entry.kind === 'animated') {
            names = Object.keys(entry.spec.animations || { play: null });
        }
        animSelect.disabled = names.length <= 1;
        for (const n of names) {
            const opt = document.createElement('option');
            opt.value = n;
            opt.textContent = n;
            animSelect.appendChild(opt);
        }
        // Carry the current pick across asset switches when possible; else
        // default to the first available animation.
        if (playback.anim && names.indexOf(playback.anim) >= 0) {
            animSelect.value = playback.anim;
        } else {
            playback.anim = names[0];
            animSelect.value = names[0];
        }
    }

    function bindTransport() {
        if (playToggle) {
            playToggle.addEventListener('click', () => setPaused(!playback.paused));
        }
        if (speedButtons) {
            for (const b of speedButtons.querySelectorAll('.speed-btn')) {
                b.addEventListener('click', () => setSpeed(parseFloat(b.dataset.speed)));
            }
        }
        if (animSelect) {
            animSelect.addEventListener('change', () => {
                playback.anim = animSelect.value;
                if (CURRENT) startLivePreview(CURRENT);
            });
        }
        setPaused(false);
        setSpeed(1);
    }

    // ---------- Save bar (PNG / WebM / GIF buttons in windowed mode) -----

    const saveBar    = document.getElementById('save-bar');
    const saveStatus = document.getElementById('save-status');

    // Per-asset enablement: PNG works for any asset that has a sheet
    // canvas surface (i.e. anything we can render); WebM/GIF need an
    // animated or sheet asset since they encode multi-frame content.
    function refreshSaveBar() {
        if (!saveBar) return;
        const entry = CURRENT ? REGISTRY[CURRENT] : null;
        const canPng  = !!entry;
        const canAnim = !!entry && (entry.kind === 'animated' || entry.kind === 'sheet' || entry.kind === 'scene');
        for (const btn of saveBar.querySelectorAll('.save-btn')) {
            const action = btn.dataset.action;
            btn.disabled = (action === 'png') ? !canPng : !canAnim;
        }
    }

    function bindSaveBar() {
        if (!saveBar) return;
        for (const btn of saveBar.querySelectorAll('.save-btn')) {
            btn.addEventListener('click', () => runSaveAction(btn.dataset.action));
        }
        refreshSaveBar();
    }

    function runSaveAction(action) {
        if (!CURRENT) { saveStatus.textContent = 'no asset loaded'; return; }
        try {
            let r;
            if (action === 'png')  r = save();
            if (action === 'webm') r = saveVideo();
            if (action === 'gif')  r = saveGif();
            saveStatus.textContent = r ? `wrote ${r.path || r.png}` : 'done';
        } catch (e) {
            saveStatus.textContent = `error: ${e.message}`;
            console.log('save action', action, 'failed:', e.message);
        }
    }

    // Load + render + start live preview. Used by picker clicks and by
    // hot-reload after a file edit.
    function selectAsset(name) {
        try {
            load(name);
            render(name);
            refreshPickerSelection();
            refreshTransport();
            refreshSaveBar();
            startLivePreview(name);
            // Windowed-mode race: the canvas pipeline can latch onto the
            // previous asset's layout box when processing this frame's draw
            // commands, so the SHEET (and INSPECT) keep showing the prior
            // asset's pixels. Re-rendering one rAF later runs after layout
            // has published the new canvas dimensions, so the surface
            // re-clears at the correct size and replays cleanly.
            if (isWindowed()) {
                requestAnimationFrame(() => {
                    if (CURRENT === name) {
                        // Same invalidation as the save path: render() rebuilds
                        // the capture SceneGraph for scene assets, which kills
                        // the refs the live-preview rAF loop was holding.
                        // Restart the loop so the stage keeps animating.
                        try { render(name); } catch (e) {}
                        try { startLivePreview(name); } catch (e) {}
                    }
                });
            }
        } catch (err) {
            console.log(`selectAsset(${name}) failed:`, err.message);
            status.textContent = `error: ${err.message}`;
        }
    }

    function startWatcher() {
        if (watcher || typeof require !== 'function') return;
        let fs;
        try { fs = require('fs'); } catch (e) { return; }
        if (typeof fs.watch !== 'function') {
            watchStatus.textContent = 'fs.watch unavailable';
            return;
        }
        try {
            watcher = fs.watch('assets', { recursive: false }, (event, filename) => {
                if (!filename || !filename.endsWith('.js')) return;
                const name = filename.slice(0, -3);
                // Debounce per-file: editors often fire 2-3 events per save.
                if (watchTimers[name]) clearTimeout(watchTimers[name]);
                watchTimers[name] = setTimeout(() => {
                    delete watchTimers[name];
                    handleAssetChange(name, event);
                }, 80);
            });
            watcher.on('error', err => {
                watchStatus.textContent = `watch error: ${err.message}`;
            });
            watchStatus.textContent = 'watching assets/';
        } catch (e) {
            watchStatus.textContent = `watch failed: ${e.message}`;
        }
    }

    function handleAssetChange(name, event) {
        // Refresh picker so newly-added files show up and removed ones drop.
        rebuildPicker();
        const after = discoverAssets();

        // If the changed file is the one we're viewing, reload it. Otherwise
        // just register it so the picker stays current.
        if (name === CURRENT) {
            selectAsset(name);
            watchStatus.textContent = `reloaded ${name}`;
        } else if (after.includes(name)) {
            // New asset added while not selected — load to populate kind tag.
            try { load(name); refreshPickerSelection(); } catch (e) {}
            watchStatus.textContent = `${event}: ${name}`;
        } else {
            watchStatus.textContent = `${event}: ${name}`;
        }
    }

    // Defer init: when bro-headless executes app.js, the headless globals
    // (`screenshotCanvas`, `screenshot`, etc.) are not yet installed at script
    // load time but are by the time the next task runs. Deferring also lets
    // us run after the global expose block below — load() evals asset
    // modules in global scope and needs window.defineSheet to exist.
    function initWindowed() {
        if (!isWindowed()) return;
        rebuildPicker();
        bindTransport();
        bindSaveBar();
        const names = discoverAssets();
        if (names.length > 0) selectAsset(names[0]);
        startWatcher();
    }

    setTimeout(initWindowed, 0);

    // ---------- Expose globals ------------------------------------------

    window.defineSheet     = defineSheet;
    window.defineTileset   = defineTileset;
    window.defineImage     = defineImage;
    window.defineNineSlice = defineNineSlice;
    window.defineAtlas     = defineAtlas;
    window.defineAnimated  = defineAnimated;
    window.defineScene     = defineScene;
    window.listAssets    = listAssets;
    window.load          = load;
    window.render        = render;
    window.save          = save;
    window.saveVideo     = saveVideo;
    window.saveGif       = saveGif;
    window.preview       = preview;
    window.previewMap    = previewMap;

})();
