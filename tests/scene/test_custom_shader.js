// Test custom shader modules on static mesh materials — mesh.setShader /
// setShaderModule installs user brokernel modules (vertex / fragment / light
// hooks composed with mesh_main.bk), setShaderUniform drives numeric uniform
// struct fields live, clearShader restores the default pipeline. Exercises the
// program cache in src/scene/vulkan/draw/scene_mesh_drawer.cpp
// (SceneMeshDrawer::program — identical sources share one program), the custom
// sub-pass routing in scene_renderer.cpp (incl. the "custom shader wins over
// unlit" rule), the eager-compile error path (invalid module throws, node keeps
// rendering default), and the setShader/setShaderUniform/clearShader bindings
// in src/bronze_host/native_scene_shaders.cpp.

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

// Fragment hook that zeroes the PBR inputs and emits a pure emissive color,
// so the output pixel is exactly the emissive value (linear tonemap, base
// color 0 kills diffuse/ambient; residual spec off a black dielectric is
// a couple of counts at most).
const emitFrag = (r, g, b) => `
    module hook;
    import mesh;
    pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
        var out = s;
        out.base_color = vec3<f32>(0.0);
        out.metallic  = 0.0;
        out.roughness = 1.0;
        out.emissive  = vec3<f32>(${r.toFixed(1)}, ${g.toFixed(1)}, ${b.toFixed(1)});
        return out;
    }`;

const probe = freshScene(64);
if (!probe.scene) {
    missingGpuContext('scene');
} else {
    dropScene(probe);
    const SIZE = 128;

    // =====================================================================
    // Fragment hook forces a known color.
    // =====================================================================
    {
        const s = freshScene(SIZE);
        const m = s.scene.createMesh({ mesh: 'box', color: '#ff0000' });
        assert(m.hasShader === false, 'hasShader is false before setShader');
        m.setShader({ module: emitFrag(0, 1, 0) });
        assert(m.hasShader === true, 'hasShader is true after setShader');
        const img = s.scene.captureFrame();
        const r = centerChannel(img, 0, 8), g = centerChannel(img, 1, 8);
        const b = centerChannel(img, 2, 8);
        assert(g > 200, `fragment hook forces green (${g})`);
        assert(r < 40 && b < 40,
            `fragment hook killed the red base color (r=${r} b=${b})`);
        dropScene(s);
    }

    // =====================================================================
    // Vertex hook displaces geometry: a +1.5 X offset moves the box from
    // the center to the right side of the frame.
    // =====================================================================
    {
        const s = freshScene(SIZE);
        const m = s.scene.createMesh({ mesh: 'box', color: '#ff0000' });
        const before = s.scene.captureFrame();
        assert(centerChannel(before, 0, 8) > 60, 'box starts at the center');

        m.setShader({ module: `
            module hook;
            import mesh;
            pub fn vertex(v: mesh::Vertex) -> mesh::Vertex {
                var out = v;
                out.pos.x += 1.5;
                return out;
            }` });
        const after = s.scene.captureFrame();
        assert(centerChannel(after, 0, 8) < 10,
            `vertex hook moved the box off center (${centerChannel(after, 0, 8)})`);
        // fov 60 @ z=4: x=+1.5 -> NDC ~0.65 -> box center ~px 105, half ~14px.
        const cy = SIZE / 2;
        const right = regionChannel(after, 0, 98, cy - 4, 112, cy + 4);
        assert(right > 60, `displaced box renders on the right (${right})`);
        dropScene(s);
    }

    // =====================================================================
    // User uniforms: initial values via setShader({uniforms}), live updates
    // via setShaderUniform between captures.
    // =====================================================================
    {
        const s = freshScene(SIZE);
        const m = s.scene.createMesh({ mesh: 'box', color: '#ffffff' });
        m.setShader({
            module: `
                module hook;
                import mesh;
                struct Params {
                    tint: vec3<f32>,
                    gain: f32,
                }
                @binding(4, 0) var params: uniform<Params>;
                pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
                    var out = s;
                    out.base_color = vec3<f32>(0.0);
                    out.roughness = 1.0;
                    out.emissive  = params.tint * params.gain;
                    return out;
                }`,
            uniforms: { tint: [1, 0, 0], gain: 1.0 },
        });
        const a = s.scene.captureFrame();
        assert(centerChannel(a, 0, 8) > 200 && centerChannel(a, 2, 8) < 40,
            'initial uniforms render red');

        m.setShaderUniform('tint', [0, 0, 1]);
        advanceTime(16);
        const b = s.scene.captureFrame();
        assert(centerChannel(b, 2, 8) > 200 && centerChannel(b, 0, 8) < 40,
            'setShaderUniform retints to blue live');

        m.setShaderUniform('gain', 0.0);
        const c = s.scene.captureFrame();
        assert(centerChannel(c, 2, 8) < 10,
            `scalar uniform update applies (${centerChannel(c, 2, 8)})`);

        // Name/value validation.
        let threw = false;
        try { m.setShaderUniform('', 1.0); } catch (e) { threw = true; }
        assert(threw, 'setShaderUniform rejects an empty name');
        threw = false;
        try { m.setShaderUniform('tint', 'red'); } catch (e) { threw = true; }
        assert(threw, 'setShaderUniform rejects non-numeric values');
        dropScene(s);
    }

    // =====================================================================
    // Invalid module throws with a compiler message; the mesh keeps rendering
    // with its previous (default) pipeline.
    // =====================================================================
    {
        const s = freshScene(SIZE);
        const m = s.scene.createMesh({ mesh: 'box', color: '#ff0000' });
        let msg = null;
        try {
            m.setShader({ module: 'module hook; pub fn fragment(this is not brokernel' });
        } catch (e) {
            msg = String(e.message || e);
        }
        assert(msg !== null, 'invalid module throws');
        assert(msg.length > 10, `error carries the compiler log (${msg})`);
        assert(m.hasShader === false, 'failed setShader leaves no shader installed');
        const img = s.scene.captureFrame();
        assert(centerChannel(img, 0, 8) > 60,
            'mesh still renders default after a failed compile');

        // A failed set also never clobbers a previously working shader.
        m.setShader({ module: emitFrag(0, 1, 0) });
        try { m.setShader({ module: 'module hook; garbage(' }); } catch (e) {}
        assert(m.hasShader === true, 'failed setShader keeps the previous shader');
        assert(centerChannel(s.scene.captureFrame(), 1, 8) > 200,
            'previous shader still renders after a failed re-set');
        dropScene(s);
    }

    // =====================================================================
    // clearShader restores default rendering.
    // =====================================================================
    {
        const s = freshScene(SIZE);
        const m = s.scene.createMesh({ mesh: 'box', color: '#ff0000' });
        m.setShader({ module: emitFrag(0, 1, 0) });
        assert(centerChannel(s.scene.captureFrame(), 1, 8) > 200, 'shader active');
        m.clearShader();
        assert(m.hasShader === false, 'hasShader false after clearShader');
        const img = s.scene.captureFrame();
        assert(centerChannel(img, 0, 8) > 60 && centerChannel(img, 1, 8) < 40,
            'clearShader restores the default red box');
        dropScene(s);
    }

    // =====================================================================
    // Shared program: two meshes with identical sources both render (one
    // cached program) and keep independent uniform values; a shaderless
    // mesh in the same scene is unaffected (no state bleed).
    // =====================================================================
    {
        const s = freshScene(SIZE);
        const tintMod = `
            module hook;
            import mesh;
            struct Params { tint: vec3<f32> }
            @binding(4, 0) var params: uniform<Params>;
            pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
                var out = s;
                out.base_color = vec3<f32>(0.0);
                out.roughness = 1.0;
                out.emissive  = params.tint;
                return out;
            }`;
        const a = s.scene.createMesh({ mesh: 'box', x: -1.4, color: '#ffffff' });
        const b = s.scene.createMesh({ mesh: 'box', x:  1.4, color: '#ffffff' });
        s.scene.createMesh({ mesh: 'box', y: -1.4, color: '#ff0000' });  // plain
        a.setShader({ module: tintMod, uniforms: { tint: [0, 1, 0] } });
        b.setShader({ module: tintMod, uniforms: { tint: [0, 0, 1] } });

        const img = s.scene.captureFrame();
        const cy = SIZE / 2;
        // fov 60 @ z=4: x=±1.4 -> NDC ~±0.61 -> box centers ~px 25 / 103.
        const leftG  = regionChannel(img, 1, 18, cy - 4, 32, cy + 4);
        const leftB  = regionChannel(img, 2, 18, cy - 4, 32, cy + 4);
        const rightB = regionChannel(img, 2, 96, cy - 4, 110, cy + 4);
        const rightG = regionChannel(img, 1, 96, cy - 4, 110, cy + 4);
        assert(leftG > 200 && leftB < 40,
            `shared program, mesh A keeps its own uniform (g=${leftG} b=${leftB})`);
        assert(rightB > 200 && rightG < 40,
            `shared program, mesh B keeps its own uniform (b=${rightB} g=${rightG})`);
        // Plain mesh below stays red — no shader/uniform state bleeds over.
        const cx = SIZE / 2;
        // y=-1.4 -> NDC ~-0.61 -> py ~103 (y flips).
        const plainR = regionChannel(img, 0, cx - 7, 96, cx + 7, 110);
        const plainG = regionChannel(img, 1, cx - 7, 96, cx + 7, 110);
        assert(plainR > 60 && plainG < 30,
            `shaderless mesh unaffected next to shaded ones (r=${plainR} g=${plainG})`);
        dropScene(s);
    }

    // =====================================================================
    // Unlit interaction rule: a custom shader forces the lit pass — the
    // fragment hook runs even on an unlit-flagged mesh (unlit would
    // otherwise early-return before the hook). clearShader restores the
    // unlit overlay behavior.
    // =====================================================================
    {
        const s = freshScene(SIZE);
        const m = s.scene.createMesh({ mesh: 'box', color: '#ff0000', unlit: true });
        m.setShader({ module: emitFrag(0, 1, 0) });
        const withShader = s.scene.captureFrame();
        assert(centerChannel(withShader, 1, 8) > 200,
            'custom shader overrides unlit (hook runs, mesh renders lit)');
        m.clearShader();
        const cleared = s.scene.captureFrame();
        assert(centerChannel(cleared, 0, 8) > 200 && centerChannel(cleared, 1, 8) < 40,
            'clearShader restores unlit overlay rendering at authored color');
        dropScene(s);
    }

    // =====================================================================
    // API validation: setShader argument shapes.
    // =====================================================================
    {
        const s = freshScene(SIZE);
        const m = s.scene.createMesh({ mesh: 'box' });
        for (const badCall of [
            () => m.setShader(),
            () => m.setShader({}),
            () => m.setShader({ module: 42 }),
            () => m.setShader({ module: emitFrag(1, 0, 0), uniforms: { tint: 'bad' } }),
        ]) {
            let threw = false;
            try { badCall(); } catch (e) { threw = true; }
            assert(threw, `setShader rejects bad arguments (${badCall})`);
        }
        assert(m.hasShader === false, 'rejected calls install nothing');
        dropScene(s);
    }

    console.log('custom shader tests passed');
}
