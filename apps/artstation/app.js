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

    function configurePixel(ctx) {
        // Pixel-perfect: integer coords, no smoothing.
        ctx.imageSmoothingEnabled = false;
    }

    // Resize a canvas to the target asset size and clear to bg.
    function resetCanvas(canvas, w, h, bg) {
        canvas.width  = w;
        canvas.height = h;
        canvas.style.width  = w + 'px';
        canvas.style.height = h + 'px';
        const ctx = canvas.getContext('2d');
        configurePixel(ctx);
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
        const ctx = resetCanvas(sheetCanvas, w, h, spec.bg);

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
        const ctx = resetCanvas(sheetCanvas, w, h, spec.bg);

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

    function render(name) {
        name = name || CURRENT;
        if (!name) throw new Error('nothing loaded — call load("name") first');
        CURRENT = name;
        const entry = REGISTRY[name];
        if (entry.kind === 'sheet') renderSheet(name);
        else if (entry.kind === 'tileset') renderTileset(name);
        else throw new Error('unknown kind: ' + entry.kind);
        updateInfo(name);
        // Auto-update the inspect canvas at default scale.
        inspect();
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
        const cellW = (entry.kind === 'sheet' ? spec.frameWidth : spec.tileSize) * scale;
        const cellH = (entry.kind === 'sheet' ? spec.frameHeight : spec.tileSize) * scale;
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

        const meta = (entry.kind === 'sheet')
            ? {
                kind: 'sheet',
                src: `${name}.png`,
                frameWidth: entry.spec.frameWidth,
                frameHeight: entry.spec.frameHeight,
                cols: entry.spec.cols,
                rows: entry.spec.rows,
                frameCount: entry.spec.frames.length,
                animations: entry.spec.animations || {},
              }
            : {
                kind: 'tileset',
                src: `${name}.png`,
                tileSize: entry.spec.tileSize,
                cols: entry.spec.cols,
                tileCount: entry.spec.tiles.length,
              };

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
        if (entry.kind !== 'sheet') {
            throw new Error('preview() only works for sheets');
        }
        if (!stageScene) {
            stageScene = stageCanvas.getContext('scene');
        }
        if (stageSprite) { stageSprite.destroy(); stageSprite = null; }

        const spec = entry.spec;
        // Use the freshly-saved PNG. If save() hasn't been called we still
        // have to use the on-disk file — caller's responsibility.
        stageSprite = stageScene.createSprite({
            src: `output/${CURRENT}.png`,
            sheet: {
                frameWidth: spec.frameWidth,
                frameHeight: spec.frameHeight,
                columns: spec.cols,
                rows: spec.rows,
            },
            animations: spec.animations || {},
            x: stageCanvas.width / 2,
            y: stageCanvas.height / 2,
            // Scale up so a 32px sprite is visible on a 320x240 stage.
            width: spec.frameWidth * 4,
            height: spec.frameHeight * 4,
        });
        if (animName && spec.animations && spec.animations[animName]) {
            stageSprite.play(animName);
        }
        status.textContent = `previewing ${CURRENT}` + (animName ? `:${animName}` : '');
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
        } else {
            const s = entry.spec;
            lines.push(`tile:   ${s.tileSize} x ${s.tileSize}`);
            lines.push(`tiles:  ${s.tiles.length} (cols=${s.cols})`);
        }
        const sz = (entry.kind === 'sheet' ? sheetSize : tilesetSize)(entry.spec);
        lines.push(`png:    ${sz.w} x ${sz.h} px`);
        info.textContent = lines.join('\n');
    }

    // ---------- Expose globals ------------------------------------------

    window.defineSheet   = defineSheet;
    window.defineTileset = defineTileset;
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
