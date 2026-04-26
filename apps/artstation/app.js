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
    const inspectCanvas = document.getElementById('inspect');
    const stageCanvas   = document.getElementById('stage');
    const status        = document.getElementById('status');
    const info          = document.getElementById('info');

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

    function buildManifest(name, entry) {
        const spec = entry.spec;
        switch (entry.kind) {
            case 'sheet': return {
                kind: 'sheet', src: `${name}.png`,
                frameWidth: spec.frameWidth, frameHeight: spec.frameHeight,
                cols: spec.cols, rows: spec.rows,
                frameCount: spec.frames.length,
                animations: spec.animations || {},
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
            case 'animated': {
                const lay = entry.layout || animatedSize(spec);
                // Default to a single 'play' animation covering all frames.
                // User can override via spec.animations and the override
                // wins. Manifest output mirrors defineSheet so consumers
                // (scene.createSprite) treat it as an ordinary sprite sheet.
                const allFrames = [];
                for (let i = 0; i < lay.n; i++) allFrames.push(i);
                const anims = spec.animations || {
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

    // Resize a canvas to the target asset size and clear to bg.
    function resetCanvas(canvas, w, h, bg, pixel) {
        canvas.width  = w;
        canvas.height = h;
        canvas.style.width  = w + 'px';
        canvas.style.height = h + 'px';
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
            default: throw new Error('unknown kind: ' + entry.kind);
        }
        updateInfo(name);
        if (entry.kind === 'sheet' || entry.kind === 'tileset' ||
            entry.kind === 'animated') {
            inspect();
        } else {
            // No inspect view for this kind — wipe any stale pixels left over
            // from the previously-selected asset so the panel doesn't lie.
            resetCanvas(inspectCanvas, inspectCanvas.width, inspectCanvas.height, null);
        }
    }

    // ---------- Inspect view (integer-scaled copy of the sheet) ---------

    // Re-renders the current asset into the inspect canvas at integer scale
    // by replaying the frame/tile draw functions through a scaled transform
    // (canvas-to-canvas drawImage isn't supported on the scene path).
    // Default scale picks the largest integer factor that fits ~384px.
    function inspect(scale) {
        if (!CURRENT) return;
        const sw = sheetCanvas.width, sh = sheetCanvas.height;
        if (scale === undefined) {
            scale = Math.max(1, Math.floor(384 / Math.max(sw, sh)));
        }
        const w = sw * scale, h = sh * scale;
        const ctx = resetCanvas(inspectCanvas, w, h, null);
        configurePixel(ctx);

        const entry = REGISTRY[CURRENT];
        const spec = entry.spec;

        if (entry.kind === 'sheet') {
            const fw = spec.frameWidth, fh = spec.frameHeight;
            for (let i = 0; i < spec.frames.length; i++) {
                const fn = spec.frames[i];
                if (!fn) continue;
                const cx = (i % spec.cols) * fw * scale;
                const cy = Math.floor(i / spec.cols) * fh * scale;
                ctx.save();
                ctx.beginPath();
                ctx.rect(cx, cy, fw * scale, fh * scale);
                ctx.clip();
                ctx.translate(cx, cy);
                ctx.scale(scale, scale);
                try { fn(ctx, fw, fh, i); } catch (e) {}
                ctx.restore();
            }
        } else if (entry.kind === 'animated') {
            // Replay the procedural animation through virtual frame indices,
            // same as renderAnimated but at integer scale into inspect.
            const lay = entry.layout || animatedSize(spec);
            const fw = spec.frameWidth, fh = spec.frameHeight;
            const dt = 1 / spec.fps;
            const state = (typeof spec.init === 'function') ? (spec.init() || {}) : {};
            for (let i = 0; i < lay.n; i++) {
                const cx = (i % lay.cols) * fw * scale;
                const cy = Math.floor(i / lay.cols) * fh * scale;
                ctx.save();
                ctx.beginPath();
                ctx.rect(cx, cy, fw * scale, fh * scale);
                ctx.clip();
                ctx.translate(cx, cy);
                ctx.scale(scale, scale);
                try { spec.frame(ctx, fw, fh, i * dt, dt, state); } catch (e) {}
                ctx.restore();
            }
        } else {
            const ts = spec.tileSize;
            for (let i = 0; i < spec.tiles.length; i++) {
                const fn = spec.tiles[i];
                if (!fn) continue;
                const cx = (i % spec.cols) * ts * scale;
                const cy = Math.floor(i / spec.cols) * ts * scale;
                ctx.save();
                ctx.beginPath();
                ctx.rect(cx, cy, ts * scale, ts * scale);
                ctx.clip();
                ctx.translate(cx, cy);
                ctx.scale(scale, scale);
                try { fn(ctx, ts, i); } catch (e) {}
                ctx.restore();
            }
        }

        // Faint cyan grid lines on frame/tile boundaries.
        ctx.fillStyle = 'rgba(0,255,255,0.35)';
        const cellW = (entry.kind === 'tileset' ? spec.tileSize : spec.frameWidth) * scale;
        const cellH = (entry.kind === 'tileset' ? spec.tileSize : spec.frameHeight) * scale;
        for (let x = cellW; x < w; x += cellW) ctx.fillRect(x, 0, 1, h);
        for (let y = cellH; y < h; y += cellH) ctx.fillRect(0, y, w, 1);
    }

    // ---------- Save (PNG via headless screenshot crop) -----------------

    // Saves the sheet canvas to output/<name>.png and writes a sidecar
    // <name>.json with the manifest (frame size, animations, etc) so
    // game code can load both with one fetch.
    //
    // The actual PNG write happens via headless's `screenshot(path,sel)`
    // helper which crops to the element's bounding box. We size the
    // canvas pixel-exact to the asset, so the cropped PNG is 1:1.
    function save(name) {
        name = name || CURRENT;
        if (!name) throw new Error('nothing to save');
        if (typeof screenshotCanvas !== 'function') {
            throw new Error('save() requires headless mode (screenshotCanvas missing)');
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
        }
        let sz;
        if      (entry.kind === 'sheet')    sz = sheetSize(entry.spec);
        else if (entry.kind === 'tileset')  sz = tilesetSize(entry.spec);
        else if (entry.kind === 'atlas')    { const l = entry.layout || atlasLayout(entry.spec); sz = { w: l.width, h: l.height }; }
        else if (entry.kind === 'animated') { const l = entry.layout || animatedSize(entry.spec); sz = { w: l.w, h: l.h }; }
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

    function isWindowed() {
        // Headless installs screenshotCanvas; if it's missing we're windowed.
        return typeof screenshotCanvas === 'undefined';
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
            liveAnimSpec  = defaultAnimation(meta);
            liveStartTime = performance.now();
            const loop = (t) => {
                if (!liveAnimSpec || CURRENT !== name) return;
                const elapsed = (t - liveStartTime) / 1000;
                const idx = Math.floor(elapsed * liveAnimSpec.fps);
                const len = liveAnimSpec.frames.length;
                const frameIdx = liveAnimSpec.loop
                    ? liveAnimSpec.frames[idx % len]
                    : liveAnimSpec.frames[Math.min(idx, len - 1)];
                drawSheetFrameLive(spec, frameIdx);
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
                liveAccumS += realDt;
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
                livePreviewRAF = requestAnimationFrame(loop);
            };
            livePreviewRAF = requestAnimationFrame(loop);
            return;
        }
    }

    // Load + render + start live preview. Used by picker clicks and by
    // hot-reload after a file edit.
    function selectAsset(name) {
        try {
            load(name);
            render(name);
            refreshPickerSelection();
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
                        try { render(name); } catch (e) {}
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
    window.listAssets    = listAssets;
    window.load          = load;
    window.render        = render;
    window.save          = save;
    window.preview       = preview;
    window.previewMap    = previewMap;
    // Headless installs its own `inspect()` (layout debugger) after page
    // load, so we expose ours under a different name to avoid the clobber.
    window.inspectArt    = inspect;

})();
