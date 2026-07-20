// User sampler slots on custom shaders — the three capabilities layered on
// top of test_custom_shader.js's plain setShaderTexture coverage:
//
//   (a) `mipmap: true` builds a real mip chain with GL_LINEAR_MIPMAP_LINEAR
//       minification, so textureLod() at a FRACTIONAL level blends the two
//       bracketing levels (flushUserTex in src/scene/mesh_node.cpp). Without
//       it there is only level 0 and GL clamps every lod to it.
//   (b) `x`/`y` turn a set into a glTexSubImage2D sub-rectangle write that
//       touches only that region and keeps the mip chain coherent
//       (MeshNode::updateCustomShaderTexture).
//   (c) The shadow pass binds user samplers exactly as the color pass does
//       (uploadUserTextures in scene_renderer_shadow.cpp), so a vertex chunk
//       that displaces from a height texture casts the DISPLACED silhouette
//       instead of sampling an unbound unit and casting the rest silhouette.

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
        sc.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
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
    uniform sampler2D u_tex;
    uniform vec2 u_uv;
    uniform float u_lod;
    void userFragment(inout vec3 baseColor, inout vec3 normal,
                      inout float metallic, inout float roughness,
                      inout vec3 emissive, inout float alpha) {
        baseColor = vec3(0.0);
        metallic  = 0.0;
        roughness = 1.0;
        emissive  = vec3(textureLod(u_tex, u_uv, u_lod).r);
    }`;

// Sample the texel at integer coords (tx, ty) of an N-wide texture at `lod`.
// Uses the exact texel CENTER so level-0 bilinear filtering resolves to that
// one texel with zero weight on its neighbours.
function probe(scene, node, N, tx, ty, lod) {
    node.setShaderUniform('u_uv', [(tx + 0.5) / N, (ty + 0.5) / N]);
    node.setShaderUniform('u_lod', lod);
    return centerBrightness(scene.captureFrame(), 5);
}

const boot = makeScene(64);
if (!boot.scene) {
    console.log('scene context not available (no GPU) — skipping shader texture test');
} else {
    dropScene(boot);
    const SIZE = 128;

    // =====================================================================
    // (a) Fractional textureLod blends two mip levels.
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
        quad.setShader({ fragment: probeFrag, uniforms: { u_uv: [0.5, 0.5], u_lod: 0 } });

        // --- no mipmaps: lod is clamped to level 0, so lod 1 == lod 0 ---
        quad.setShaderTexture('u_tex', { width: N, height: N, data: stripes });
        const flatL0 = probe(scn, quad, N, 32, 16, 0.0);
        const flatL1 = probe(scn, quad, N, 32, 16, 1.0);
        assert(flatL0 > 230,
            `unmipmapped level 0 reads the bright row (${flatL0.toFixed(1)})`);
        assert(Math.abs(flatL1 - flatL0) < 8,
            `without a chain every lod clamps to level 0 ` +
            `(lod0=${flatL0.toFixed(1)}, lod1=${flatL1.toFixed(1)})`);

        // --- mipmap: true -> level 1 exists and is the 0.5 average ---
        quad.setShaderTexture('u_tex',
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
        quad.setShader({ fragment: probeFrag, uniforms: { u_uv: [0.5, 0.5], u_lod: 0 } });
        quad.setShaderTexture('u_tex', { width: N, height: N, data: flat });

        const base = probe(scn, quad, N, 10, 10, 0.0);
        assert(Math.abs(base - 63.75) < 6,
            `flat 0.25 texture reads back ~64 (${base.toFixed(1)})`);

        // Write a 4x4 block of 1.0 at (8,8) -> covers texels x,y in [8,12).
        const block = new Float32Array(4 * 4).fill(1.0);
        quad.setShaderTexture('u_tex',
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
        quad.setShaderTexture('u_tex', { x: 28, y: 28, width: 8, height: 8, data: stray });
        quad.setShaderTexture('u_tex', { x: -4, y: 0, width: 8, height: 8, data: stray });
        quad.setShaderTexture('u_nope', { x: 0, y: 0, width: 8, height: 8, data: stray });

        const corner = probe(scn, quad, N, 31, 31, 0.0);
        assert(Math.abs(corner - base) < 6,
            `out-of-bounds sub-rect wrote nothing (${corner.toFixed(1)})`);
        const stillInside = probe(scn, quad, N, 10, 10, 0.0);
        assert(stillInside > 230,
            'rejected sub-updates left the accepted one intact');

        // A sub-write into a MIPMAPPED slot must regenerate the chain, or
        // level 1 would keep serving pre-write texels forever.
        quad.setShaderTexture('u_tex',
            { width: N, height: N, data: flat, mipmap: true });
        const mipBefore = probe(scn, quad, N, 10, 10, 1.0);
        assert(Math.abs(mipBefore - 63.75) < 8,
            `mipmapped flat texture: level 1 also ~64 (${mipBefore.toFixed(1)})`);
        quad.setShaderTexture('u_tex',
            { x: 8, y: 8, width: 4, height: 4, data: block });
        const mipAfter = probe(scn, quad, N, 10, 10, 1.0);
        assert(mipAfter > 230,
            `level 1 rebuilt after the sub-write (${mipBefore.toFixed(1)} -> ` +
            `${mipAfter.toFixed(1)})`);

        dropScene(s);
    }

    // =====================================================================
    // (c) A vertex chunk displacing from a user texture casts the displaced
    // shadow. Same rig as test_custom_shader_variants.js's uniform-driven
    // shadow case: the key light travels (0,-1,1)/sqrt2, so a blocker at
    // height y shadows the ground around z ~ y. Driving the displacement
    // from a TEXTURE rather than a uniform is the point — before the shadow
    // pass bound user samplers, the blocker moved in the color pass while
    // its silhouette stayed put.
    // =====================================================================
    {
        const s = makeScene(256);
        const scn = s.scene;
        const FOV = 40, EYE = [0, 1, 8];
        scn.setCamera({ fov: FOV, near: 0.1, far: 100, position: EYE, target: [0, 1, 0] });
        scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
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
            vertex: `
                uniform sampler2D u_disp;
                void userVertex(inout vec3 pos, inout vec3 normal, inout vec2 uv) {
                    pos.x += 1.5 * textureLod(u_disp, vec2(0.5), 0.0).r;
                }`,
        });
        blocker.cullMargin = 1.5;   // displacement exceeds the mesh AABB

        const D = 8;
        const zeros = new Float32Array(D * D);   // 0.0
        blocker.setShaderTexture('u_disp', { width: D, height: D, data: zeros });
        const atRest = scn.captureFrame();
        assert(patchBrightness(atRest, ox, oy, 3) < baseMoved * 0.8,
            'zero-valued height texture leaves the shadow at rest');

        // Full re-upload of 1.0 -> blocker and shadow both move +1.5 x.
        const ones = new Float32Array(D * D).fill(1.0);
        blocker.setShaderTexture('u_disp', { width: D, height: D, data: ones });
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
        blocker.setShaderTexture('u_disp',
            { x: 0, y: 0, width: D, height: D, data: zeroBlock });
        const back = scn.captureFrame();
        assert(patchBrightness(back, ox, oy, 3) < baseMoved * 0.8,
            'sub-rect update moved the shadow back');
        assert(patchBrightness(back, mx, my, 3) > baseMoved * 0.8,
            'the displaced spot is lit again after the sub-rect update');

        dropScene(s);
    }

    console.log('shader texture mips/sub-update/shadow test passed');
}
