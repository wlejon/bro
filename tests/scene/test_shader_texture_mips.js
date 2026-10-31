// User sampler slots on custom shaders — the capabilities layered on top of
// test_custom_shader.js's plain setShaderTexture coverage:
//
//   (a) `mipmap: true` builds a real mip chain with linear-mipmap-linear
//       minification, so sample_lod() at a FRACTIONAL level blends the two
//       bracketing levels. Without it there is only level 0 and every lod
//       clamps to it.
//   (b) `x`/`y` turn a set into a sub-rectangle write that touches only
//       that region and keeps the mip chain coherent
//       (MeshNode::updateCustomShaderTexture).
//   (c) The shadow pass binds user samplers exactly as the color pass does
//       (vulkan/shadow/pass_shadow.cpp), so a vertex hook
//       that displaces from a height texture casts the DISPLACED silhouette
//       instead of sampling an unbound unit and casting the rest silhouette.
//   (d) `layers` makes a slot a texture2d_array of slices and `custom::repeat`
//       wraps its coordinates (MeshNode::setCustomShaderTextureArray).
//   (e) `buffer` makes a KernelBuffer the texels, copied every frame
//       (MeshNode::setCustomShaderTextureBuffer), as floats or `format`
//       'unorm16'.
//   (f) `depth` makes a slot a texture3d, filtered across its z slices.

function patchBrightness(img, cx, cy, r) {
    let sum = 0, n = 0;
    for (let y = cy - r; y <= cy + r; y++) {
        for (let x = cx - r; x <= cx + r; x++) {
            if (x < 0 || y < 0 || x >= img.width || y >= img.height) continue;
            const i = (y * img.width + x) * 4;
            sum += (img.data[i] + img.data[i + 1] + img.data[i + 2]) / 3;
            n++;
        }
    }
    return n ? sum / n : 0;
}

function centerBrightness(img, r) {
    return patchBrightness(img, Math.floor(img.width / 2),
                           Math.floor(img.height / 2), r);
}

function makeScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    const sc = cv.getContext('scene');
    if (sc) {
        sc.setCamera({ fov: 60, near: 0.1, far: 100,
                       position: [0, 0, 4], target: [0, 0, 0] });
        // Linear tonemap, gamma 1 -> the emissive value the hook writes is
        // the pixel value, so a probe reads back the sampled texel directly.
        sc.setToneMap({ mode: 'linear', gamma: 1.0 });
    }
    return { canvas: cv, scene: sc };
}

function dropScene(s) {
    document.body.removeChild(s.canvas);
    flush();
}

// Probe shader: samples ONE texel chosen by uniform, not by the mesh's UVs,
// so a readback depends on nothing but the texture contents and the lod —
// no assumptions about the primitive's UV layout or screen coverage.
const probeFrag = `
    module hook;
    import mesh;
    import custom;
    struct Params {
        uv: vec2<f32>,
        lod: f32,
    }
    @binding(4, 0) var params: uniform<Params>;
    @binding(4, 1) var tex: texture2d<vec4<f32>>;
    pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
        var out = s;
        out.base_color = vec3<f32>(0.0);
        out.metallic  = 0.0;
        out.roughness = 1.0;
        out.emissive  = vec3<f32>(tex.sample_lod(custom::linear, params.uv, params.lod).x);
        return out;
    }`;

// Sample the texel at integer coords (tx, ty) of an N-wide texture at `lod`.
// Uses the exact texel CENTER so level-0 bilinear filtering resolves to that
// one texel with zero weight on its neighbours.
function probe(scene, node, N, tx, ty, lod) {
    node.setShaderUniform('uv', [(tx + 0.5) / N, (ty + 0.5) / N]);
    node.setShaderUniform('lod', lod);
    return centerBrightness(scene.captureFrame(), 5);
}

const boot = makeScene(64);
if (!boot.scene) {
    missingGpuContext('scene');
} else {
    dropScene(boot);
    const SIZE = 128;

    // =====================================================================
    // (a) Fractional sample_lod blends two mip levels.
    //
    // Level 0 is horizontal stripes with period 2 (even rows 1.0, odd 0.0),
    // so every 2x2 box-filtered block averages to 0.5 and level 1 is
    // UNIFORMLY 0.5. Probing an even row therefore gives 1.0 at lod 0 and
    // 0.5 at lod 1 — a large, exactly-known gap for the blend to sit in.
    // =====================================================================
    {
        const s = makeScene(SIZE);
        const scn = s.scene;
        const N = 64;
        const stripes = new Float32Array(N * N);
        for (let y = 0; y < N; y++)
            for (let x = 0; x < N; x++) stripes[y * N + x] = (y % 2 === 0) ? 1.0 : 0.0;

        const quad = scn.createMesh({ mesh: 'box', color: 'white' });
        quad.setShader({ module: probeFrag, uniforms: { uv: [0.5, 0.5], lod: 0 } });

        // --- no mipmaps: lod is clamped to level 0, so lod 1 == lod 0 ---
        quad.setShaderTexture('tex', { width: N, height: N, data: stripes });
        const flatL0 = probe(scn, quad, N, 32, 16, 0.0);
        const flatL1 = probe(scn, quad, N, 32, 16, 1.0);
        assert(flatL0 > 230,
            `unmipmapped level 0 reads the bright row (${flatL0.toFixed(1)})`);
        assert(Math.abs(flatL1 - flatL0) < 8,
            `without a chain every lod clamps to level 0 ` +
            `(lod0=${flatL0.toFixed(1)}, lod1=${flatL1.toFixed(1)})`);

        // --- mipmap: true -> level 1 exists and is the 0.5 average ---
        quad.setShaderTexture('tex',
            { width: N, height: N, data: stripes, mipmap: true });
        const l0 = probe(scn, quad, N, 32, 16, 0.0);
        const l1 = probe(scn, quad, N, 32, 16, 1.0);
        assert(l0 > 230,
            `mipmapped level 0 still reads the bright row (${l0.toFixed(1)})`);
        assert(Math.abs(l1 - 127.5) < 12,
            `level 1 is the box-filtered stripe average ~0.5 (${l1.toFixed(1)})`);
        assert(l0 - l1 > 80,
            `the two levels differ enough to detect a blend (${(l0 - l1).toFixed(1)})`);

        // The payoff: a fractional lod must land between them, at the
        // linearly interpolated value. A nearest-mip filter would snap to
        // one level and fail this.
        const lHalf = probe(scn, quad, N, 32, 16, 0.5);
        assert(Math.abs(lHalf - (l0 + l1) / 2) < 8,
            `lod 0.5 blends level 0 and level 1 ` +
            `(got ${lHalf.toFixed(1)}, expected ${((l0 + l1) / 2).toFixed(1)})`);

        const lQuarter = probe(scn, quad, N, 32, 16, 0.25);
        assert(Math.abs(lQuarter - (0.75 * l0 + 0.25 * l1)) < 8,
            `lod 0.25 weights level 0 three-to-one ` +
            `(got ${lQuarter.toFixed(1)}, ` +
            `expected ${(0.75 * l0 + 0.25 * l1).toFixed(1)})`);
        assert(lQuarter > lHalf + 20,
            'lod 0.25 sits nearer level 0 than lod 0.5 does');

        dropScene(s);
    }

    // =====================================================================
    // (b) Sub-rectangle updates touch only their region.
    // =====================================================================
    {
        const s = makeScene(SIZE);
        const scn = s.scene;
        const N = 32;
        const flat = new Float32Array(N * N).fill(0.25);   // -> ~64

        const quad = scn.createMesh({ mesh: 'box', color: 'white' });
        quad.setShader({ module: probeFrag, uniforms: { uv: [0.5, 0.5], lod: 0 } });
        quad.setShaderTexture('tex', { width: N, height: N, data: flat });

        const base = probe(scn, quad, N, 10, 10, 0.0);
        assert(Math.abs(base - 63.75) < 6,
            `flat 0.25 texture reads back ~64 (${base.toFixed(1)})`);

        // Write a 4x4 block of 1.0 at (8,8) -> covers texels x,y in [8,12).
        const block = new Float32Array(4 * 4).fill(1.0);
        quad.setShaderTexture('tex',
            { x: 8, y: 8, width: 4, height: 4, data: block });

        const inside = probe(scn, quad, N, 10, 10, 0.0);
        assert(inside > 230,
            `sub-rect interior took the new value (${inside.toFixed(1)})`);

        // Neighbours on every side of the rect, plus a far corner, must be
        // untouched — this is what separates a sub-write from a re-upload.
        for (const [px, py, where] of [[7, 10, 'left of'], [12, 10, 'right of'],
                                       [10, 7, 'above'],   [10, 12, 'below'],
                                       [0, 0, 'far from'], [31, 31, 'opposite corner from']]) {
            const v = probe(scn, quad, N, px, py, 0.0);
            assert(Math.abs(v - base) < 6,
                `texel (${px},${py}) ${where} the rect is unchanged ` +
                `(${v.toFixed(1)} vs ${base.toFixed(1)})`);
        }

        // Rejected sub-updates: out of bounds, negative origin, and an
        // unknown slot. All must warn-and-ignore, leaving the texture
        // exactly as it was — never a clipped partial write.
        const stray = new Float32Array(8 * 8).fill(0.0);
        quad.setShaderTexture('tex', { x: 28, y: 28, width: 8, height: 8, data: stray });
        quad.setShaderTexture('tex', { x: -4, y: 0, width: 8, height: 8, data: stray });
        quad.setShaderTexture('nope', { x: 0, y: 0, width: 8, height: 8, data: stray });

        const corner = probe(scn, quad, N, 31, 31, 0.0);
        assert(Math.abs(corner - base) < 6,
            `out-of-bounds sub-rect wrote nothing (${corner.toFixed(1)})`);
        const stillInside = probe(scn, quad, N, 10, 10, 0.0);
        assert(stillInside > 230,
            'rejected sub-updates left the accepted one intact');

        // A sub-write into a MIPMAPPED slot must regenerate the chain, or
        // level 1 would keep serving pre-write texels forever.
        quad.setShaderTexture('tex',
            { width: N, height: N, data: flat, mipmap: true });
        const mipBefore = probe(scn, quad, N, 10, 10, 1.0);
        assert(Math.abs(mipBefore - 63.75) < 8,
            `mipmapped flat texture: level 1 also ~64 (${mipBefore.toFixed(1)})`);
        quad.setShaderTexture('tex',
            { x: 8, y: 8, width: 4, height: 4, data: block });
        const mipAfter = probe(scn, quad, N, 10, 10, 1.0);
        assert(mipAfter > 230,
            `level 1 rebuilt after the sub-write (${mipBefore.toFixed(1)} -> ` +
            `${mipAfter.toFixed(1)})`);

        dropScene(s);
    }

    // =====================================================================
    // (c) A vertex hook displacing from a user texture casts the displaced
    // shadow.
    // =====================================================================
    {
        const s = makeScene(256);
        const scn = s.scene;
        const FOV = 40, EYE = [0, 1, 8];
        scn.setCamera({ fov: FOV, near: 0.1, far: 100, position: EYE, target: [0, 1, 0] });
        scn.setToneMap({ mode: 'linear', gamma: 1.0 });
        function project(x, y, z) {
            const d = EYE[2] - z;
            const k = 128 / (d * Math.tan((FOV / 2) * Math.PI / 180));
            return [Math.round(128 + x * k), Math.round(128 - (y - 1) * k)];
        }
        const key = scn.createLight({
            type: 'directional', direction: [0, -0.7071, 0.7071],
            color: [1, 1, 1], intensity: 2.5,
        });
        key.castsShadow = true;
        scn.createLight({ type: 'directional', direction: [0, 0, -1],
                          color: [1, 1, 1], intensity: 1.5 });
        scn.createMesh({ mesh: 'plane', halfW: 6, halfD: 6, color: 'white',
                         castsShadow: false, y: 0 });
        const blocker = scn.createMesh({ mesh: 'box', color: 'red', y: 1.5 });

        const [ox, oy] = project(0, 0, 1.75);      // shadow at rest
        const [mx, my] = project(1.5, 0, 1.75);    // shadow once displaced

        const base = scn.captureFrame();
        const baseOrig = patchBrightness(base, ox, oy, 3);
        const baseMoved = patchBrightness(base, mx, my, 3);
        assert(baseOrig < baseMoved * 0.8,
            `undisplaced blocker shadows the rest spot ` +
            `(${baseOrig.toFixed(1)} vs ${baseMoved.toFixed(1)})`);

        // Displacement = 1.5 * the sampled texel. Start at 0 so the shader
        // is installed but the silhouette should not have moved yet.
        blocker.setShader({
            module: `
                module hook;
                import mesh;
                import custom;
                @binding(4, 1) var disp: texture2d<vec4<f32>>;
                pub fn vertex(v: mesh::Vertex) -> mesh::Vertex {
                    var out = v;
                    out.pos.x += 1.5 * disp.sample_lod(custom::linear, vec2<f32>(0.5), 0.0).x;
                    return out;
                }`,
        });
        blocker.cullMargin = 1.5;   // displacement exceeds the mesh AABB

        const D = 8;
        const zeros = new Float32Array(D * D);   // 0.0
        blocker.setShaderTexture('disp', { width: D, height: D, data: zeros });
        const atRest = scn.captureFrame();
        assert(patchBrightness(atRest, ox, oy, 3) < baseMoved * 0.8,
            'zero-valued height texture leaves the shadow at rest');

        // Full re-upload of 1.0 -> blocker and shadow both move +1.5 x.
        const ones = new Float32Array(D * D).fill(1.0);
        blocker.setShaderTexture('disp', { width: D, height: D, data: ones });
        const disp = scn.captureFrame();
        const dOrig = patchBrightness(disp, ox, oy, 3);
        const dMoved = patchBrightness(disp, mx, my, 3);

        const [bx, by] = project(1.5, 1.5, 0);
        assert(patchBrightness(disp, bx, by, 2) > 40,
            'color pass: blocker drew at its displaced position');
        assert(dMoved < baseMoved * 0.8,
            `shadow pass: displaced blocker shadows the moved spot ` +
            `(${baseMoved.toFixed(1)} -> ${dMoved.toFixed(1)})`);
        assert(dOrig > baseOrig * 1.2,
            `rest spot re-lit once the shadow moved ` +
            `(${baseOrig.toFixed(1)} -> ${dOrig.toFixed(1)})`);

        // A SUB-rect write must reach the shadow pass too — both passes
        // consume the same staged queue, so neither may miss an update.
        const zeroBlock = new Float32Array(D * D);
        blocker.setShaderTexture('disp',
            { x: 0, y: 0, width: D, height: D, data: zeroBlock });
        const back = scn.captureFrame();
        assert(patchBrightness(back, ox, oy, 3) < baseMoved * 0.8,
            'sub-rect update moved the shadow back');
        assert(patchBrightness(back, mx, my, 3) > baseMoved * 0.8,
            'the displaced spot is lit again after the sub-rect update');

        dropScene(s);
    }

    // =====================================================================
    // (d) `layers` makes the slot a texture2d_array of that many slices, one
    // after another in `data`; `custom::repeat` wraps coordinates instead of
    // clamping them (`custom::linear`).
    // =====================================================================
    {
        const s = makeScene(SIZE);
        const scn = s.scene;
        const N = 8, L = 3;
        const slices = new Float32Array(N * N * L);
        for (let l = 0; l < L; l++) slices.fill(0.25 * (l + 1), l * N * N, (l + 1) * N * N);
        slices[0] = 1.0;
        const quad = scn.createMesh({ mesh: 'box', color: 'white' });
        quad.setShader({
            module: `
                module hook;
                import mesh;
                import custom;
                struct Params {
                    at: vec3<f32>,
                    rep: f32,
                }
                @binding(4, 0) var params: uniform<Params>;
                @binding(4, 1) var arr: texture2d_array<vec4<f32>>;
                pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
                    var out = s;
                    out.base_color = vec3<f32>(0.0);
                    out.metallic  = 0.0;
                    out.roughness = 1.0;
                    let r = select(params.rep > 0.5,
                                   arr.sample_lod(custom::repeat, params.at, 0.0).x,
                                   arr.sample_lod(custom::linear, params.at, 0.0).x);
                    out.emissive  = vec3<f32>(r);
                    return out;
                }`,
            uniforms: { at: [0.5, 0.5, 0], rep: 0 },
        });
        const at = (u, v, layer) => {
            quad.setShaderUniform('at', [u, v, layer]);
            return centerBrightness(scn.captureFrame(), 5);
        };
        const centre = (N / 2 + 0.5) / N, past = 1 + 0.5 / N;

        quad.setShaderTexture('arr', { width: N, height: N, layers: L, data: slices });
        for (let l = 0; l < L; l++) {
            const v = at(centre, centre, l);
            assert(Math.abs(v - 255 * 0.25 * (l + 1)) < 6,
                `slice ${l} reads its own value (${v.toFixed(1)})`);
        }
        const clamped = at(past, past, 0);
        assert(Math.abs(clamped - 63.75) < 6,
            `without repeat a coordinate past 1 clamps to the far corner (${clamped.toFixed(1)})`);

        quad.setShaderTexture('arr', { width: N, height: N, layers: L, data: slices, repeat: true });
        quad.setShaderUniform('rep', 1);
        const wrapped = at(past, past, 0);
        assert(wrapped > 245,
            `with repeat it wraps onto texel (0, 0) (${wrapped.toFixed(1)})`);
        const deep = at(centre, centre, 2);
        assert(Math.abs(deep - 191.25) < 6,
            `re-staging with repeat keeps every slice (${deep.toFixed(1)})`);

        let threw = false;
        try { quad.setShaderTexture('arr', { width: N, height: N, layers: L, data: new Float32Array(N * N) }); }
        catch (e) { threw = e instanceof RangeError; }
        assert(threw, 'data shorter than width * height * layers is a RangeError');

        dropScene(s);
    }

    // =====================================================================
    // (e) `buffer` makes a KernelBuffer the slot's texels: two channels,
    // read as .xy, slice after slice, copied every frame the node draws, so
    // a write to the buffer shows in the next frame.
    // =====================================================================
    if (bro.kernel && bro.kernel.available !== false) {
        const s = makeScene(SIZE);
        const scn = s.scene;
        const N = 8, L = 2;
        const texels = new Float32Array(N * N * L * 2);
        for (let l = 0; l < L; l++)
            for (let i = 0; i < N * N; i++) {
                texels[(l * N * N + i) * 2] = 0.25 * (l + 1);
                texels[(l * N * N + i) * 2 + 1] = 0.5;
            }
        texels[N * N * 2 + 1] = 1.0;
        const buffer = bro.kernel.createBuffer(texels);
        const quad = scn.createMesh({ mesh: 'box', color: 'white' });
        quad.setShader({
            module: `
                module hook;
                import mesh;
                import custom;
                struct Params {
                    at: vec3<f32>,
                    green: f32,
                }
                @binding(4, 0) var params: uniform<Params>;
                @binding(4, 1) var field: texture2d_array<vec4<f32>>;
                pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
                    var out = s;
                    out.base_color = vec3<f32>(0.0);
                    out.metallic  = 0.0;
                    out.roughness = 1.0;
                    let val = field.sample_lod(custom::repeat, params.at, 0.0);
                    out.emissive  = vec3<f32>(select(params.green > 0.5, val.y, val.x));
                    return out;
                }`,
            uniforms: { at: [0.5, 0.5, 0], green: 0 },
        });
        const at = (u, v, layer, green) => {
            quad.setShaderUniform('at', [u, v, layer]);
            quad.setShaderUniform('green', green ? 1 : 0);
            return centerBrightness(scn.captureFrame(), 5);
        };
        const centre = (N / 2 + 0.5) / N, past = 1 + 0.5 / N;
        quad.setShaderTexture('field', { buffer, width: N, height: N, layers: L, channels: 2, repeat: true });
        for (let l = 0; l < L; l++) {
            const r = at(centre, centre, l, false), g = at(centre, centre, l, true);
            assert(Math.abs(r - 255 * 0.25 * (l + 1)) < 6 && Math.abs(g - 127.5) < 6,
                `slice ${l} reads its texels from the buffer (${r.toFixed(1)}, ${g.toFixed(1)})`);
        }
        const wrapped = at(past, past, 1, true);
        assert(wrapped > 245, `with repeat it wraps onto texel (0, 0) (${wrapped.toFixed(1)})`);

        buffer.write(new Float32Array([0.75, 0.75]), (N * N + (N / 2) * N + N / 2) * 8);
        const rewritten = at(centre, centre, 1, false);
        assert(Math.abs(rewritten - 191.25) < 6,
            `a write to the buffer shows in the next frame (${rewritten.toFixed(1)})`);

        let threw = false;
        try { quad.setShaderTexture('field', { buffer, width: N, height: N, layers: L + 1, channels: 2 }); }
        catch (e) { threw = e instanceof RangeError; }
        assert(threw, 'a buffer smaller than the texels is a RangeError');

        // format 'unorm16': two 16-bit channels a 32-bit word, read as 0 to 1.
        const words = new Int32Array(N * N * L);
        for (let l = 0; l < L; l++)
            words.fill((Math.round(65535 * 0.25 * (l + 1)) | (Math.round(65535 * 0.75) << 16)), l * N * N, (l + 1) * N * N);
        const packed = bro.kernel.createBuffer(words);
        quad.setShaderTexture('field', { buffer: packed, width: N, height: N, layers: L, channels: 2, format: 'unorm16' });
        for (let l = 0; l < L; l++) {
            const r = at(centre, centre, l, false), g = at(centre, centre, l, true);
            assert(Math.abs(r - 255 * 0.25 * (l + 1)) < 6 && Math.abs(g - 191.25) < 6,
                `unorm16 slice ${l} reads its halves as 0 to 1 (${r.toFixed(1)}, ${g.toFixed(1)})`);
        }
        threw = false;
        try { quad.setShaderTexture('field', { buffer: packed, width: N, height: N, layers: L, channels: 2, format: 'unorm12' }); }
        catch (e) { threw = e instanceof RangeError; }
        assert(threw, 'an unknown format is a RangeError');

        dropScene(s);
    }

    // =====================================================================
    // (f) `depth` makes the slot a 3D texture of that many z slices, read
    // through a texture3d and filtered across them by the hardware.
    // =====================================================================
    {
        const s = makeScene(SIZE);
        const scn = s.scene;
        const N = 4, D = 4;
        const slices = new Float32Array(N * N * D);
        for (let l = 0; l < D; l++) slices.fill(0.25 * (l + 1), l * N * N, (l + 1) * N * N);
        const quad = scn.createMesh({ mesh: 'box', color: 'white' });
        quad.setShader({
            module: `
                module hook;
                import mesh;
                import custom;
                struct Params {
                    at: vec3<f32>,
                    rep: f32,
                }
                @binding(4, 0) var params: uniform<Params>;
                @binding(4, 1) var vol: texture3d<vec4<f32>>;
                pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
                    var out = s;
                    out.base_color = vec3<f32>(0.0);
                    out.metallic  = 0.0;
                    out.roughness = 1.0;
                    let r = select(params.rep > 0.5,
                                   vol.sample_lod(custom::repeat, params.at, 0.0).x,
                                   vol.sample_lod(custom::linear, params.at, 0.0).x);
                    out.emissive  = vec3<f32>(r);
                    return out;
                }`,
            uniforms: { at: [0.5, 0.5, 0.5], rep: 0 },
        });
        const at = (z) => {
            quad.setShaderUniform('at', [0.5, 0.5, z]);
            return centerBrightness(scn.captureFrame(), 5);
        };
        const sliceZ = (l) => (l + 0.5) / D;
        for (const source of ['data', 'buffer']) {
            if (source === 'buffer' && !(bro.kernel && bro.kernel.available !== false)) continue;
            const opts = source === 'data' ? { data: slices } : { buffer: bro.kernel.createBuffer(slices) };
            quad.setShaderTexture('vol', { width: N, height: N, depth: D, ...opts });
            quad.setShaderUniform('rep', 0);
            for (let l = 0; l < D; l++) {
                const v = at(sliceZ(l));
                assert(Math.abs(v - 255 * 0.25 * (l + 1)) < 6, `${source}: slice ${l} reads its own value (${v.toFixed(1)})`);
            }
            const mid = at(0.5 * (sliceZ(1) + sliceZ(2)));
            assert(Math.abs(mid - 255 * 0.625) < 6, `${source}: between slices 1 and 2 the hardware blends them (${mid.toFixed(1)})`);
            const clamped = at(1 + 0.5 / D);
            assert(Math.abs(clamped - 255) < 6, `${source}: without repeat z past 1 clamps to the last slice (${clamped.toFixed(1)})`);
            quad.setShaderTexture('vol', { width: N, height: N, depth: D, repeat: true, ...opts });
            quad.setShaderUniform('rep', 1);
            const wrapped = at(1 + 0.5 / D);
            assert(Math.abs(wrapped - 63.75) < 6, `${source}: with repeat z past 1 wraps onto slice 0 (${wrapped.toFixed(1)})`);
        }

        let threw = null;
        try { quad.setShaderTexture('vol', { width: N, height: N, depth: D, layers: D, data: slices }); } catch (e) { threw = e; }
        assert(threw instanceof TypeError, 'layers and depth together are a TypeError');
        threw = null;
        try { quad.setShaderTexture('vol', { width: N, height: N, depth: D, data: new Float32Array(N * N) }); } catch (e) { threw = e; }
        assert(threw instanceof RangeError, 'data shorter than width * height * depth is a RangeError');

        dropScene(s);
    }

    console.log('shader texture mips/sub-update/shadow/array/3D test passed');
}
