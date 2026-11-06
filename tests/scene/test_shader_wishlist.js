// Integration test for brokernel + bro scene/tile/kernel wishlist features:
//   1. Built-in `tiles` module & global module resolver (`bro.kernel.registerModule`,
//      `bro.kernel.resolve`) in scene shader hooks and `bro.kernel.compile`.
//   2. Scene shader `@knob const` overrides (`{ knobs }`, `node.setShaderKnobs`,
//      `scene.reloadShaders`).
//   3. Selective tile cache invalidation (`cache.invalidate(layers?)`, `batch.layers`,
//      `batch.hasLayer`).
//   4. Named struct varyings across `vertex(inout v, out varyings)` ->
//      `fragment(v, varyings, inout s)`.
//   5. Shader static & pipeline executable stats (`kernel.stats()`, `node.shaderStats()`,
//      `perf.shaderStats(node)`) and `scene.setDebugView('cost')`.
//   6. Scene fragment shader `probe("name", val)` and `scene.probe(x, y)`.

function regionChannel(img, ch, x0, y0, x1, y1) {
    let sum = 0, n = 0;
    for (let y = y0; y < y1; y++) {
        for (let x = x0; x < x1; x++) {
            sum += img.data[(y * img.width + x) * 4 + ch];
            n++;
        }
    }
    return n ? sum / n : 0;
}

function centerChannel(img, ch, r) {
    const cx = Math.floor(img.width / 2), cy = Math.floor(img.height / 2);
    return regionChannel(img, ch, cx - r, cy - r, cx + r, cy + r);
}

function freshScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    const sc = cv.getContext('scene');
    if (sc) {
        sc.setCamera({ fov: 60, near: 0.1, far: 100,
                       position: [0, 0, 4], target: [0, 0, 0] });
        sc.setToneMap({ mode: 'linear', gamma: 1.0 });
    }
    return { canvas: cv, scene: sc };
}

function dropScene(s) {
    document.body.removeChild(s.canvas);
    flush();
}

const probeCtx = freshScene(64);
if (!probeCtx.scene) {
    missingGpuContext('scene');
} else {
    dropScene(probeCtx);
    const SIZE = 128;

    // =========================================================================
    // 1 & 2. Built-in `tiles` module, `registerModule` / `resolve`, `@knob`,
    //        `setShaderKnobs`, and `scene.reloadShaders()`.
    // =========================================================================
    {
        let resolvedScale = 1.0;
        bro.kernel.registerModule('wl_color', `
            module wl_color;
            pub fn base_tint() -> vec3<f32> {
                return vec3<f32>(1.0, 0.0, 0.0);
            }
        `);
        bro.kernel.resolve((name) => {
            if (name === 'wl_scale') {
                return `
                    module wl_scale;
                    pub fn gain() -> f32 { return ${resolvedScale.toFixed(2)}; }
                `;
            }
            return undefined;
        });

        const s = freshScene(SIZE);
        const m = s.scene.createMesh({ mesh: 'box', color: '#ffffff' });
        // Note: imports `tiles`, `wl_color`, and `wl_scale` without passing `modules`!
        m.setShader({
            module: `
                module hook;
                import mesh;
                import tiles;
                import wl_color;
                import wl_scale;

                @knob const USE_GREEN: f32 = 0.0;
                @knob const BLUE_BOOST: f32 = 0.0;

                pub fn fragment(v: mesh::Vertex, inout s: mesh::Surface) {
                    s.base_color = vec3<f32>(0.0);
                    s.metallic = 0.0;
                    s.roughness = 1.0;
                    let base = select(USE_GREEN > 0.5, vec3<f32>(0.0, 1.0, 0.0), wl_color::base_tint());
                    s.emissive = (base + vec3<f32>(0.0, 0.0, BLUE_BOOST)) * wl_scale::gain();
                }
            `,
            knobs: { USE_GREEN: 0.0, BLUE_BOOST: 0.0 },
        });

        const img1 = s.scene.captureFrame();
        assert(centerChannel(img1, 0, 8) > 200 && centerChannel(img1, 1, 8) < 30,
            `initial registered module + default knobs render red (r=${centerChannel(img1, 0, 8)})`);

        // Flip USE_GREEN knob via setShaderKnobs
        m.setShaderKnobs({ USE_GREEN: 1.0, BLUE_BOOST: 0.0 });
        const img2 = s.scene.captureFrame();
        assert(centerChannel(img2, 1, 8) > 200 && centerChannel(img2, 0, 8) < 30,
            `setShaderKnobs switches output to green (g=${centerChannel(img2, 1, 8)})`);

        // Update registered module + reloadShaders()
        m.setShaderKnobs({ USE_GREEN: 0.0, BLUE_BOOST: 0.0 });
        bro.kernel.registerModule('wl_color', `
            module wl_color;
            pub fn base_tint() -> vec3<f32> {
                return vec3<f32>(0.0, 0.0, 1.0);
            }
        `);
        s.scene.reloadShaders();
        const img3 = s.scene.captureFrame();
        assert(centerChannel(img3, 2, 8) > 200 && centerChannel(img3, 0, 8) < 30,
            `reloadShaders picks up updated registered module (b=${centerChannel(img3, 2, 8)})`);

        dropScene(s);
    }

    // =========================================================================
    // 3. Selective tile cache invalidation: `cache.invalidate(layers?)`,
    //    `batch.layers`, `batch.hasLayer(name)`.
    // =========================================================================
    if (bro.kernel.available && bro.tiles && bro.kernel.gpu) {
        const T = 8;
        const kFill = bro.kernel.compile(`
            import tiles;
            @kernel @grid(3)
            fn fill_layer(g: grid<3>, table: read_store<vec4<i32>, 1>, pool: rw_store<f32, 3>, val: f32) {
                let rec = tiles::record(table, g.z());
                pool[g.x(), g.y(), rec.slot] = val;
            }
        `).kernel('fill_layer');

        let heightVal = 0.2;
        let colorVal = 0.8;
        let seenBatchLayers = [];
        let heightDispatchCount = 0;
        let colorDispatchCount = 0;

        const cache = bro.tiles.createCache({
            domain: { type: 'plane', origin: [0, 0], size: 32, roots: [1, 1] },
            tileSize: T,
            capacity: 32,
            maxLevel: 2,
            pixelsPerCell: 4,
            layers: { height: 4, color: 4 },
            producePerUpdate: 64,
            produce(batch) {
                seenBatchLayers.push(batch.layers.slice());
                if (batch.hasLayer('height')) {
                    heightDispatchCount++;
                    kFill.dispatch([this.cells, this.cells, batch.count], batch.table, this.layer('height'), heightVal);
                }
                if (batch.hasLayer('color')) {
                    colorDispatchCount++;
                    kFill.dispatch([this.cells, this.cells, batch.count], batch.table, this.layer('color'), colorVal);
                }
            },
        });

        const view = { position: [16, 10, 16], fov: 60, viewportHeight: 256 };
        for (let i = 0; i < 32 && (i === 0 || cache.pending > 0); i++) {
            cache.update(view);
            advanceTime(16);
        }
        assert(cache.pending === 0, 'two-layer tile cache settles');
        assert(seenBatchLayers.length > 0 &&
               seenBatchLayers[0].includes('height') &&
               seenBatchLayers[0].includes('color'),
               `initial production includes both layers (${JSON.stringify(seenBatchLayers[0])})`);

        const s = freshScene(64);
        const quad = s.scene.createMesh({ mesh: 'box', color: 'white' });
        quad.setShader({
            module: `
                module hook;
                import mesh;
                import custom;
                struct Params { at: vec3<f32> }
                @binding(4, 0) var params: uniform<Params>;
                @binding(4, 1) var h_tex: texture2d_array<vec4<f32>>;
                @binding(4, 2) var c_tex: texture2d_array<vec4<f32>>;
                pub fn fragment(v: mesh::Vertex, inout s: mesh::Surface) {
                    s.base_color = vec3<f32>(0.0);
                    s.metallic = 0.0;
                    s.roughness = 1.0;
                    let h = h_tex.sample_lod(custom::linear, params.at, 0.0).x;
                    let c = c_tex.sample_lod(custom::linear, params.at, 0.0).x;
                    s.emissive = vec3<f32>(h, c, 0.0);
                }
            `,
            uniforms: { at: [0.25, 0.25, 0] },
        });
        quad.setShaderTexture('h_tex', { buffer: cache.layer('height'), width: T, height: T, layers: cache.capacity, atlas: 2 });
        quad.setShaderTexture('c_tex', { buffer: cache.layer('color'), width: T, height: T, layers: cache.capacity, atlas: 2 });

        const slot0 = cache.drawn[0].slot;
        const sampleSlot = (slot) => {
            const cell = slot % 4, c = (T / 2 + 0.5) / T;
            quad.setShaderUniform('at', [((cell % 2) + c) / 2, (Math.floor(cell / 2) + c) / 2, Math.floor(slot / 4)]);
            const img = s.scene.captureFrame();
            return [centerChannel(img, 0, 4), centerChannel(img, 1, 4)];
        };

        const [hBefore, cBefore] = sampleSlot(slot0);
        assert(Math.abs(hBefore - 255 * 0.2) < 8 && Math.abs(cBefore - 255 * 0.8) < 8,
            `initial h=${hBefore} (~51), c=${cBefore} (~204)`);

        // Also clobber the CPU/GPU buffer of height without invalidating height,
        // and change colorVal to 0.4, then selectively invalidate ONLY ['color']!
        heightVal = 0.9;
        colorVal = 0.4;
        seenBatchLayers = [];
        const hCountBefore = heightDispatchCount;
        const cCountBefore = colorDispatchCount;

        cache.invalidate(['color']);
        for (let i = 0; i < 32 && (i === 0 || cache.pending > 0); i++) {
            cache.update(view);
            advanceTime(16);
        }
        assert(cache.pending === 0, 'selective invalidate(["color"]) settles');
        assert(seenBatchLayers.length > 0 &&
               seenBatchLayers.every((l) => l.length === 1 && l[0] === 'color'),
               `selective invalidate sets batch.layers to ["color"] (${JSON.stringify(seenBatchLayers)})`);
        assert(heightDispatchCount === hCountBefore && colorDispatchCount > cCountBefore,
            'only color layer was re-dispatched');

        const [hAfter, cAfter] = sampleSlot(slot0);
        assert(Math.abs(hAfter - hBefore) < 6,
            `un-invalidated height layer preserved on GPU (before=${hBefore}, after=${hAfter})`);
        assert(Math.abs(cAfter - 255 * 0.4) < 8,
            `invalidated color layer updated on GPU (before=${cBefore}, after=${cAfter})`);

        dropScene(s);
    }

    // =========================================================================
    // 4. Named struct varyings across `vertex` -> `fragment`.
    // =========================================================================
    {
        const s = freshScene(SIZE);
        const m = s.scene.createMesh({ mesh: 'box', color: '#ffffff' });
        m.setShader({
            module: `
                module hook;
                import mesh;

                struct Varyings {
                    color_a: vec3<f32>,
                    scale: f32,
                    mode: i32,
                }

                pub fn vertex(inout v: mesh::Vertex, out varyings: Varyings) {
                    varyings.color_a = vec3<f32>(0.2, 0.8, 0.4);
                    varyings.scale = 1.0;
                    varyings.mode = 7;
                }

                pub fn fragment(v: mesh::Vertex, varyings: Varyings, inout s: mesh::Surface) {
                    s.base_color = vec3<f32>(0.0);
                    s.metallic = 0.0;
                    s.roughness = 1.0;
                    if (varyings.mode == 7) {
                        s.emissive = varyings.color_a * varyings.scale;
                    } else {
                        s.emissive = vec3<f32>(1.0, 0.0, 0.0);
                    }
                }
            `,
        });

        const img = s.scene.captureFrame();
        const r = centerChannel(img, 0, 8);
        const g = centerChannel(img, 1, 8);
        const b = centerChannel(img, 2, 8);
        assert(Math.abs(r - 255 * 0.2) < 10 && Math.abs(g - 255 * 0.8) < 10 && Math.abs(b - 255 * 0.4) < 10,
            `named struct varyings passed vec3 + f32 + i32 from vertex to fragment (r=${r}, g=${g}, b=${b})`);

        // Exceeding 24 float slots throws a clear error
        let overErr = null;
        try {
            m.setShader({
                module: `
                    module hook;
                    import mesh;
                    struct TooBig {
                        a: vec4<f32>, b: vec4<f32>, c: vec4<f32>, d: vec4<f32>,
                        e: vec4<f32>, f: vec4<f32>, g: vec4<f32>,
                    }
                    pub fn vertex(inout v: mesh::Vertex, out varyings: TooBig) {}
                `,
            });
        } catch (e) {
            overErr = String(e.message || e);
        }
        assert(overErr !== null && overErr.includes('24'),
            `oversized varying struct throws clear slot limit error (${overErr})`);

        dropScene(s);
    }

    // =========================================================================
    // 5. Static & driver shader stats (`kernel.stats()`, `node.shaderStats()`,
    //    `perf.shaderStats(node)`) and `scene.setDebugView('cost')`.
    // =========================================================================
    {
        if (bro.kernel.available) {
            const kmod = bro.kernel.compile(`
                @kernel @grid(1)
                fn saxpy(g: grid<1>, x: read_store<f32, 1>, y: rw_store<f32, 1>, a: f32) {
                    let i = g.x();
                    y[i] = a * x[i] + y[i];
                }
            `);
            const k = kmod.kernel('saxpy');
            const kst = k.stats();
            assert(kst && typeof kst === 'object', 'kernel.stats() returns an object');
            assert(kst.spirvBytes > 0 && kst.instructions > 0 && kst.arithmetic > 0,
                `kernel.stats() reports static SPIR-V counts (bytes=${kst.spirvBytes}, inst=${kst.instructions}, arith=${kst.arithmetic})`);
            assert(Array.isArray(kst.executables), 'kernel.stats().executables is an array');
            assert(Array.isArray(kst.functions) && kst.functions.length > 0 &&
                   kst.functions[0].name === 'saxpy' && kst.functions[0].share === 1 &&
                   kst.functions[0].total >= kst.functions[0].self,
                `kernel.stats().functions leads with the kernel at share 1 (${JSON.stringify(kst.functions)})`);
        }

        const s = freshScene(SIZE);
        const m = s.scene.createMesh({ mesh: 'box', color: '#ffffff' });
        m.setShader({
            module: `
                module hook;
                import mesh;
                pub fn fragment(v: mesh::Vertex, inout s: mesh::Surface) {
                    s.base_color = vec3<f32>(0.5, 0.2, 0.8);
                    s.roughness = 0.4;
                }
            `,
        });
        s.scene.captureFrame();

        const nst = m.shaderStats();
        assert(nst && nst.vertex && nst.fragment, 'node.shaderStats() returns vertex & fragment stats');
        assert(nst.fragment.spirvBytes > 0 && nst.fragment.instructions > 0 && nst.fragment.arithmetic > 0,
            `fragment shaderStats reports SPIR-V counts (bytes=${nst.fragment.spirvBytes}, inst=${nst.fragment.instructions})`);
        assert(Array.isArray(nst.fragment.executables), 'fragment.executables is an array');
        const ff = nst.fragment.functions;
        assert(Array.isArray(ff) && ff.length > 1 && ff[0].share === 1 &&
               ff.some((f) => f.name === 'hook::fragment' && f.self > 0),
            `fragment.functions attributes the stage to source functions, the hook among them (${JSON.stringify(ff && ff.slice(0, 4))})`);

        const pst = perf.shaderStats(m);
        assert(pst && pst.fragment && pst.fragment.instructions === nst.fragment.instructions,
            'perf.shaderStats(node) matches node.shaderStats()');

        // Cost heatmap debug view
        s.scene.setDebugView('cost');
        assert(s.scene.debugViews().shown === 'cost', 'debugViews().shown returns "cost"');
        const costImg = s.scene.captureFrame();
        const costSum = centerChannel(costImg, 0, 8) + centerChannel(costImg, 1, 8) + centerChannel(costImg, 2, 8);
        assert(costSum > 50, `cost debug view renders heatmap at mesh center (sum=${costSum})`);
        s.scene.setDebugView(null);
        assert(s.scene.debugViews().shown === null, 'setDebugView(null) clears debug view');

        dropScene(s);
    }

    // =========================================================================
    // 6. Scene fragment shader `probe("name", expr)` & `scene.probe(x, y)`.
    // =========================================================================
    {
        const s = freshScene(SIZE);
        const m = s.scene.createMesh({ mesh: 'box', color: '#ffffff' });
        m.setShader({
            module: `
                module hook;
                import mesh;
                pub fn fragment(v: mesh::Vertex, inout s: mesh::Surface) {
                    let p_scalar = probe("answer", 42.5);
                    let p_vec2 = probe("uv_pair", vec2<f32>(0.25, 0.75));
                    let p_vec3 = probe("tint", vec3<f32>(0.1, 0.5, 0.9));
                    s.base_color = vec3<f32>(0.0);
                    s.roughness = 1.0;
                    s.emissive = p_vec3 + vec3<f32>(p_scalar * 0.0 + p_vec2.x * 0.0);
                }
            `,
        });

        const hit = s.scene.probe(SIZE / 2, SIZE / 2);
        assert(hit !== null && typeof hit === 'object', `scene.probe at center returns an object (${JSON.stringify(hit)})`);
        assert(Math.abs(hit.answer - 42.5) < 1e-3, `probe scalar "answer" == 42.5 (${hit.answer})`);
        assert(Array.isArray(hit.uv_pair) && hit.uv_pair.length === 2 &&
               Math.abs(hit.uv_pair[0] - 0.25) < 1e-3 && Math.abs(hit.uv_pair[1] - 0.75) < 1e-3,
               `probe vec2 "uv_pair" == [0.25, 0.75] (${JSON.stringify(hit.uv_pair)})`);
        assert(Array.isArray(hit.tint) && hit.tint.length === 3 &&
               Math.abs(hit.tint[0] - 0.1) < 1e-3 && Math.abs(hit.tint[1] - 0.5) < 1e-3 && Math.abs(hit.tint[2] - 0.9) < 1e-3,
               `probe vec3 "tint" == [0.1, 0.5, 0.9] (${JSON.stringify(hit.tint)})`);

        const miss = s.scene.probe(2, 2);
        assert(miss.answer === undefined && Object.keys(miss.nodes || {}).length === 0,
               `scene.probe at empty corner records no hits (${JSON.stringify(miss)})`);

        dropScene(s);
    }

    // A probe beside buffer reads made through a generic function's buffer
    // parameter (tiles::probe(table, ...), as the planet's ground does).
    {
        const s = freshScene(SIZE);
        const m = s.scene.createMesh({ mesh: 'box', color: '#ffffff' });
        let err = null;
        try {
            m.setShader({
                module: `
                    module hook;
                    import mesh;
                    import tiles;
                    @binding(4, 19) var lut: read_buffer<vec4<i32>>;
                    pub fn fragment(v: mesh::Vertex, inout s: mesh::Surface) {
                        let slot = tiles::probe(lut, 0, 0, 0, 0);
                        s.base_color = vec3<f32>(0.0);
                        s.emissive = vec3<f32>(probe("slot", f32(slot.x) * 0.0 + 0.5));
                    }
                `,
            });
        } catch (e) { err = String(e && e.message || e); }
        assert(err === null, `a probe compiles beside a generic buffer read (${err})`);
        if (err === null) {
            const hit = s.scene.probe(SIZE / 2, SIZE / 2);
            assert(Math.abs(hit.slot - 0.5) < 1e-3, `probe "slot" == 0.5 (${JSON.stringify(hit)})`);
        }
        dropScene(s);
    }

    console.log('shader wishlist tests passed');
}
