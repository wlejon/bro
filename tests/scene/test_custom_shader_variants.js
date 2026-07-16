// Custom shaders on the non-static mesh pipelines — the skinned and
// instanced program variants plus shadow-pass displacement added on top of
// test_custom_shader.js's static coverage. Exercises the SKINNED /
// INSTANCED custom variants of ensureCustomProgram (scene_renderer_mesh.cpp),
// the grouped custom sub-passes in render3D (scene_renderer.cpp), the
// spliced shadow variants + caster routing (ensureCustomShadowProgram in
// scene_renderer_shadow.cpp — a vertex-displaced mesh casts the DISPLACED
// silhouette, fragment-only keeps the default depth program), and the
// setShader bindings accepting skinned + instanced nodes.

function regionChannel(img, ch, x0, y0, x1, y1) {
    let sum = 0, n = 0;
    for (let y = y0; y < y1; y++) {
        for (let x = x0; x < x1; x++) {
            if (x < 0 || y < 0 || x >= img.width || y >= img.height) continue;
            sum += img.data[(y * img.width + x) * 4 + ch];
            n++;
        }
    }
    return n ? sum / n : 0;
}

function patchChannel(img, ch, cx, cy, r) {
    return regionChannel(img, ch, cx - r, cy - r, cx + r + 1, cy + r + 1);
}

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

// Count pixels whose RGBA differs by more than `tol` in any channel.
function diffCount(a, b, tol) {
    let n = 0;
    for (let i = 0; i < a.data.length; i += 4) {
        if (Math.abs(a.data[i]     - b.data[i])     > tol ||
            Math.abs(a.data[i + 1] - b.data[i + 1]) > tol ||
            Math.abs(a.data[i + 2] - b.data[i + 2]) > tol ||
            Math.abs(a.data[i + 3] - b.data[i + 3]) > tol) n++;
    }
    return n;
}

function makeScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    return { canvas: cv, scene: cv.getContext('scene') };
}

function dropScene(s) {
    document.body.removeChild(s.canvas);
    flush();
}

// Fragment chunk that zeroes the PBR inputs and emits a pure emissive color
// (exact output under linear tonemap, gamma 1).
const emitFrag = (r, g, b) => `
    void userFragment(inout vec3 baseColor, inout vec3 normal,
                      inout float metallic, inout float roughness,
                      inout vec3 emissive, inout float alpha) {
        baseColor = vec3(0.0);
        metallic  = 0.0;
        roughness = 1.0;
        emissive  = vec3(${r.toFixed(1)}, ${g.toFixed(1)}, ${b.toFixed(1)});
    }`;

const probe = makeScene(64);
if (!probe.scene) {
    console.log('scene context not available (no GPU) — skipping custom shader variants test');
} else {
    dropScene(probe);

    // =====================================================================
    // Skinned: fragment hook + vertex displacement, both riding the pose.
    // 2-bone strip rig from test_skinned_mesh.js — lower half bone 0, upper
    // half bone 1 with a hinge at (0,1,0); the bend palette swings the upper
    // half to a horizontal arm at x in [-1,0], y ~= 1.
    // =====================================================================
    {
        const s = makeScene(256);
        const scn = s.scene;
        const FOV = 40, EYE = [0, 1, 8];
        scn.setCamera({ fov: FOV, near: 0.1, far: 100, position: EYE, target: [0, 1, 0] });
        scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
        scn.createLight({ type: 'directional', direction: [0, 0, -1],
                          color: [1, 1, 1], intensity: 1.5 });
        function project(x, y, z) {
            const d = EYE[2] - z;
            const k = 128 / (d * Math.tan((FOV / 2) * Math.PI / 180));
            return [Math.round(128 + x * k), Math.round(128 - (y - 1) * k)];
        }

        const ROWS = 9;
        const positions = new Float32Array(ROWS * 2 * 3);
        const normals   = new Float32Array(ROWS * 2 * 3);
        const boneW = new Float32Array(ROWS * 2 * 4);
        const boneI = new Uint32Array(ROWS * 2 * 4);
        for (let r = 0; r < ROWS; r++) {
            const y = r * 0.25;
            for (let c = 0; c < 2; c++) {
                const v = r * 2 + c;
                positions[v * 3 + 0] = c === 0 ? -0.2 : 0.2;
                positions[v * 3 + 1] = y;
                normals[v * 3 + 2] = 1;
                boneW[v * 4 + 0] = 1;
                boneI[v * 4 + 0] = y > 1.0 ? 1 : 0;
            }
        }
        const indices = new Uint32Array((ROWS - 1) * 6);
        for (let r = 0; r < ROWS - 1; r++) {
            const bl = r * 2, br = r * 2 + 1, tl = (r + 1) * 2, tr = (r + 1) * 2 + 1;
            indices.set([bl, br, tr, bl, tr, tl], r * 6);
        }

        const node = scn.createSkinnedMesh({
            positions, normals, indices, color: 'red', roughness: 0.9,
            skin: new SkinData({
                boneWeights: boneW, boneIndices: boneI, boneCount: 2,
                inverseBindMatrices: new Float32Array([
                    1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1,
                    1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1,
                ]),
            }),
        });
        assert(node.skinReady === true, 'skinned rig ready');

        const imgDefault = scn.captureFrame();
        const [sx, sy] = project(0, 0.5, 0);       // lower strip center
        assert(patchChannel(imgDefault, 0, sx, sy, 3) > 60,
            'skinned strip renders red before setShader');

        // setShader no longer throws on skinned meshes; fragment hook runs
        // through the SKINNED program variant.
        node.setShader({ fragment: emitFrag(0, 1, 0) });
        assert(node.hasShader === true, 'hasShader true on skinned node');
        const imgGreen = scn.captureFrame();
        assert(patchChannel(imgGreen, 1, sx, sy, 3) > 200 &&
               patchChannel(imgGreen, 0, sx, sy, 3) < 40,
            'fragment hook forces green on a skinned mesh');

        // Bend the rig through the real pipeline; the hook keeps rendering
        // on the POSED geometry: the horizontal arm is green, the old strip
        // top is empty.
        const skel = new Skeleton({ bones: [
            { name: 'root',  parent: -1 },
            { name: 'upper', parent: 0, localT: [0, 1, 0],
              inverseBind: [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,-1,0,1] },
        ]});
        const pose = skel.bindPose();
        const pd = pose.data;
        const s45 = Math.SQRT1_2;
        pd[10 + 3] = 0; pd[10 + 4] = 0;
        pd[10 + 5] = s45; pd[10 + 6] = s45;
        pose.data = pd;
        node.setSkinningMatrices(pose.computeSkinningMatrices(skel));

        const imgBent = scn.captureFrame();
        const [ax, ay] = project(-0.6, 1, 0);      // bent arm
        const [tx, ty] = project(0, 1.75, 0);      // former strip top
        assert(patchChannel(imgBent, 1, ax, ay, 2) > 200,
            'fragment hook renders on the posed arm (skinned variant deforms)');
        assert(patchChannel(imgBent, 1, tx, ty, 2) < 20,
            'former strip top empty while bent (not drawing bind pose)');

        // Vertex displacement runs POST-skin: +1.0 x shifts the whole posed
        // figure right, so the arm lands at x in [0,1] and vacates x ~ -0.6.
        node.setShader({
            vertex: `
                void userVertex(inout vec3 pos, inout vec3 normal, inout vec2 uv) {
                    pos.x += 1.0;
                }`,
            fragment: emitFrag(0, 1, 0),
        });
        const imgShift = scn.captureFrame();
        const [dx, dy] = project(0.4, 1, 0);       // displaced arm
        assert(patchChannel(imgShift, 1, dx, dy, 2) > 200,
            'skinned vertex displacement moves the posed arm (+1 x)');
        assert(patchChannel(imgShift, 1, ax, ay, 2) < 20,
            'original arm region vacated by the displacement');

        // clearShader restores the default skinned pipeline (red, bent, in
        // the original place).
        node.clearShader();
        assert(node.hasShader === false, 'hasShader false after clearShader');
        const imgCleared = scn.captureFrame();
        assert(patchChannel(imgCleared, 0, ax, ay, 2) > 60 &&
               patchChannel(imgCleared, 1, ax, ay, 2) < 40,
            'clearShader restores the default red bent arm');
        dropScene(s);
    }

    // =====================================================================
    // Instanced: fragment hook applies to every instance; a shaderless
    // instanced node in the same scene is unaffected; vertex displacement
    // moves all instances; clearShader restores defaults.
    // =====================================================================
    {
        const SIZE = 128;
        const s = makeScene(SIZE);
        const scn = s.scene;
        scn.setCamera({ fov: 60, near: 0.1, far: 100,
                        position: [0, 0, 4], target: [0, 0, 0] });
        scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });

        // instancesFromTransforms: px py pz qx qy qz qw scale variant
        const xf = (tx, ty) => [tx, ty, 0, 0, 0, 0, 1, 1, 0];
        const boxMesh = Mesh.box();
        const shaded = scn.createInstancedMesh({ mesh: boxMesh, color: 'red',
            instancesFromTransforms: new Float32Array([...xf(-1.2, 0), ...xf(0, 0)]) });
        const plain = scn.createInstancedMesh({ mesh: boxMesh, color: 'red',
            instancesFromTransforms: new Float32Array([...xf(1.2, 0)]) });

        assert(shaded.hasShader === false, 'instanced hasShader false initially');
        shaded.setShader({ fragment: emitFrag(0, 1, 0) });
        assert(shaded.hasShader === true, 'instanced hasShader true after setShader');

        // fov 60 @ z=4: x=±1.2 -> px ~128/2 ± 1.2*55.4 -> ~-2/64/130... use
        // measured centers: x=-1.2 -> ~px 12 (hmm, keep patches inside the
        // instance quads: half-size 0.5 box -> ~±14 px around each center).
        const k = (SIZE / 2) / Math.tan(30 * Math.PI / 180) / 4;  // px per unit
        const cx = (x) => Math.round(SIZE / 2 + x * k);
        const cy = SIZE / 2;
        const img = scn.captureFrame();
        const gA = patchChannel(img, 1, cx(-1.2), cy, 3);
        const gB = patchChannel(img, 1, cx(0), cy, 3);
        const rC = patchChannel(img, 0, cx(1.2), cy, 3);
        const gC = patchChannel(img, 1, cx(1.2), cy, 3);
        assert(gA > 200 && gB > 200,
            `fragment hook applies to every instance (${gA.toFixed(0)}, ${gB.toFixed(0)})`);
        assert(rC > 60 && gC < 30,
            `shaderless instanced node unaffected next to it (r=${rC.toFixed(0)} g=${gC.toFixed(0)})`);

        // Vertex displacement (mesh-local, pre-instance-transform): +1.0 y
        // moves every instance's box up by 1 in its own frame.
        shaded.setShader({
            vertex: `
                void userVertex(inout vec3 pos, inout vec3 normal, inout vec2 uv) {
                    pos.y += 1.0;
                }`,
            fragment: emitFrag(0, 1, 0),
        });
        const img2 = scn.captureFrame();
        const upY = Math.round(SIZE / 2 - 1.0 * k);
        assert(patchChannel(img2, 1, cx(0), upY, 3) > 200,
            'instanced vertex displacement moves instances up');
        assert(patchChannel(img2, 1, cx(0), cy, 3) < 20,
            'original instance position vacated');

        shaded.clearShader();
        assert(shaded.hasShader === false, 'instanced hasShader false after clear');
        const img3 = scn.captureFrame();
        assert(patchChannel(img3, 0, cx(0), cy, 3) > 60 &&
               patchChannel(img3, 1, cx(0), cy, 3) < 30,
            'clearShader restores default red instances');
        dropScene(s);
    }

    // =====================================================================
    // Shadow displacement: a vertex-displaced blocker casts the DISPLACED
    // silhouette. Key light travels (0,-1,1)/sqrt2, so a blocker at height
    // y shadows the ground at z ~ y. Displacing the blocker +1.5 x (via a
    // user uniform, so the shadow pass's per-caster uniform upload is
    // exercised too) must move the ground shadow with it.
    // =====================================================================
    {
        const s = makeScene(256);
        const scn = s.scene;
        const FOV = 40, EYE = [0, 1, 8];
        scn.setCamera({ fov: FOV, near: 0.1, far: 100, position: EYE, target: [0, 1, 0] });
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
        // Blocker box (unit cube) centered at (0, 1.5, 0): shadow lands on
        // the ground around (0, 0, 1.5)..(0, 0, 2).
        const blocker = scn.createMesh({ mesh: 'box', color: 'red', y: 1.5 });

        const [ox, oy] = project(0, 0, 1.75);      // shadow at rest
        const [mx, my] = project(1.5, 0, 1.75);    // shadow after +1.5 x
        const base = scn.captureFrame();
        const baseOrig = patchBrightness(base, ox, oy, 3);
        const baseMoved = patchBrightness(base, mx, my, 3);
        assert(baseOrig < baseMoved * 0.8,
            `undisplaced blocker shadows the rest spot (${baseOrig.toFixed(1)} vs ${baseMoved.toFixed(1)})`);

        // Fragment-only shader first: silhouette must be UNCHANGED (the
        // caster keeps the shared default shadow program).
        blocker.setShader({ fragment: emitFrag(0, 1, 0) });
        const fragOnly = scn.captureFrame();
        const foOrig = patchBrightness(fragOnly, ox, oy, 3);
        const foMoved = patchBrightness(fragOnly, mx, my, 3);
        assert(Math.abs(foOrig - baseOrig) < baseOrig * 0.15 + 4,
            `fragment-only shader keeps the default shadow (${baseOrig.toFixed(1)} -> ${foOrig.toFixed(1)})`);
        assert(Math.abs(foMoved - baseMoved) < baseMoved * 0.15 + 4,
            'fragment-only shader leaves the lit spot lit');

        // Uniform-driven vertex displacement: the blocker AND its shadow
        // move +1.5 x together.
        blocker.setShader({
            vertex: `
                uniform float u_dx;
                void userVertex(inout vec3 pos, inout vec3 normal, inout vec2 uv) {
                    pos.x += u_dx;
                }`,
            uniforms: { u_dx: 1.5 },
        });
        blocker.cullMargin = 1.5;   // displacement exceeds the mesh AABB
        const disp = scn.captureFrame();
        const dOrig = patchBrightness(disp, ox, oy, 3);
        const dMoved = patchBrightness(disp, mx, my, 3);
        assert(dMoved < baseMoved * 0.8,
            `displaced blocker shadows the moved spot (${baseMoved.toFixed(1)} -> ${dMoved.toFixed(1)})`);
        assert(dOrig > baseOrig * 1.2,
            `rest spot re-lit once the shadow moved (${baseOrig.toFixed(1)} -> ${dOrig.toFixed(1)})`);
        // The blocker itself moved too (sanity that the color pass agrees).
        const [bx, by] = project(1.5, 1.5, 0);
        assert(patchBrightness(disp, bx, by, 2) > 40,
            'displaced blocker visible at its new position');

        // Live uniform update drives the shadow, not just the color pass.
        blocker.setShaderUniform('u_dx', 0.0);
        const back = scn.captureFrame();
        assert(patchBrightness(back, ox, oy, 3) < baseOrig * 1.3 + 8 &&
               patchBrightness(back, mx, my, 3) > baseMoved * 0.8,
            'setShaderUniform moves the shadow back live');

        // A chunk referencing an engine uniform (uWindTime) must compile in
        // the SHADOW variant too — the mirrored declarations in shadow.vert
        // sit before the splice point, so the displaced silhouette still
        // lands (regression guard for the marker-in-comment splice bug: a
        // failed shadow compile would silently fall back to the undisplaced
        // default program and this shadow would not move).
        blocker.setShader({
            vertex: `
                void userVertex(inout vec3 pos, inout vec3 normal, inout vec2 uv) {
                    pos.x += 1.5 + 0.0 * uWindTime;
                }`,
        });
        const windRef = scn.captureFrame();
        assert(patchBrightness(windRef, mx, my, 3) < baseMoved * 0.8,
            'engine-uniform-referencing chunk still displaces the shadow');

        // clearShader restores the default pipeline + silhouette.
        blocker.clearShader();
        const cleared = scn.captureFrame();
        assert(Math.abs(patchBrightness(cleared, ox, oy, 3) - baseOrig) <
                   baseOrig * 0.2 + 6,
            'clearShader restores the undisplaced shadow');
        dropScene(s);
    }

    // =====================================================================
    // cullMargin surface: numeric round-trip on mesh + instanced, undefined
    // elsewhere.
    // =====================================================================
    {
        const s = makeScene(64);
        const m = s.scene.createMesh({ mesh: 'box' });
        assert(m.cullMargin === 0, 'cullMargin defaults to 0');
        m.cullMargin = 2.5;
        assert(Math.abs(m.cullMargin - 2.5) < 1e-6, 'cullMargin round-trips');
        m.cullMargin = -1;
        assert(m.cullMargin === 0, 'negative cullMargin clamps to 0');
        const inst = s.scene.createInstancedMesh({ mesh: Mesh.box(),
            instancesFromTransforms: new Float32Array([0,0,0, 0,0,0,1, 1, 0]) });
        inst.cullMargin = 1.25;
        assert(Math.abs(inst.cullMargin - 1.25) < 1e-6, 'instanced cullMargin round-trips');
        const light = s.scene.createLight({ type: 'directional', direction: [0,-1,0] });
        assert(light.cullMargin === undefined, 'cullMargin undefined on non-mesh nodes');
        dropScene(s);
    }

    console.log('custom shader variants tests passed');
}
