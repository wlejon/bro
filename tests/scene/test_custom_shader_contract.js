// The custom-shader module contract, by pixels: modules import `mesh` and
// `custom` and declare set-4 bindings directly.
//   - the engine varyings v.uv, v.world_pos (camera-relative), v.cam_dist and
//     the vertex inputs (v.tangent among them) answer on mesh::Vertex;
//   - a uniform struct at (4, 0) used in both stages reads the same values in
//     each;
//   - a user varying slot (v.v0, v.vi) the vertex hook writes reaches the
//     fragment hook;
//   - a hook that reads v.tangent still casts a shadow (the shadow pass
//     compiles the vertex hook too);
//   - eight textures (bindings 1..8) fit, a ninth is refused with a compile
//     error.

function freshScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    const sc = cv.getContext('scene');
    if (sc) {
        sc.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 4], target: [0, 0, 0] });
        sc.setToneMap({ mode: 'linear', gamma: 1.0 });
        sc.setAmbient({ color: [0, 0, 0] });
        sc.createLight({ type: 'directional', intensity: 0 });
    }
    return { canvas: cv, scene: sc };
}

function dropScene(s) {
    document.body.removeChild(s.canvas);
    flush();
}

function rgbAt(img, x, y) {
    const i = (y * img.width + x) * 4;
    return [img.data[i], img.data[i + 1], img.data[i + 2]];
}

const near = (a, b, tol) => Math.abs(a - b) <= tol;
const fmt = (c) => '(' + c.join(',') + ')';

// Emit `expr` (a vec3<f32>) and nothing else.
const emitting = (decls, expr) => `
    module hook;
    import mesh;
    import custom;
    ${decls}
    pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
        var out = s;
        out.base_color = vec3<f32>(0.0);
        out.metallic = 0.0;
        out.roughness = 1.0;
        out.emissive = ${expr};
        return out;
    }`;

const probe = freshScene(32);
if (!probe.scene) {
    missingGpuContext('scene');
} else {
    dropScene(probe);
    const S = 128;

    // --- engine varyings on mesh::Vertex --------------------------------------
    {
        const s = freshScene(S);
        const box = s.scene.createMesh({ mesh: Mesh.box(2, 2, 0.02), color: [1, 1, 1] });
        box.setShader({
            module: emitting('', 'vec3<f32>(v.uv.x, v.uv.y, clamp(-v.world_pos.z / 8.0, 0.0, 1.0)) * step(3.0, v.cam_dist)'),
        });
        const c = rgbAt(s.scene.captureFrame(), 64, 64);
        assert(near(c[0], 128, 12) && near(c[1], 128, 12), `v.uv is the face's uv at its centre ${fmt(c)}`);
        assert(near(c[2], 126, 12), `v.world_pos is camera-relative and v.cam_dist the eye distance ${fmt(c)}`);
        dropScene(s);
    }

    // --- one uniform block for both stages ------------------------------------
    {
        const s = freshScene(S);
        const box = s.scene.createMesh({ mesh: Mesh.box(0.6, 0.6, 0.02), color: [1, 1, 1] });
        box.setShader({
            module: `
                module hook;
                import mesh;
                struct Params {
                    shift: f32,
                    unused: f32,
                    color: vec3<f32>,
                }
                @binding(4, 0) var params: uniform<Params>;
                pub fn vertex(v: mesh::Vertex) -> mesh::Vertex {
                    var out = v;
                    out.pos.x += params.shift;
                    return out;
                }
                pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
                    var out = s;
                    out.base_color = vec3<f32>(0.0);
                    out.metallic = 0.0;
                    out.roughness = 1.0;
                    out.emissive = params.color * step(0.5, params.shift);
                    return out;
                }`,
            uniforms: { shift: 1.0, unused: 7.0, color: [0.25, 0.5, 1.0] },
        });
        const img = s.scene.captureFrame();
        const moved = rgbAt(img, 64 + 32, 64);
        const left = rgbAt(img, 64, 64);
        assert(near(moved[0], 64, 6) && near(moved[1], 128, 8) && near(moved[2], 255, 8),
               `both stages read shift and color from one block ${fmt(moved)}`);
        assert(left[0] + left[1] + left[2] < 10, `the vertex stage moved the box off centre ${fmt(left)}`);
        dropScene(s);
    }

    // --- custom varyings (v.v0, v.vi) -----------------------------------------
    {
        const s = freshScene(S);
        const box = s.scene.createMesh({ mesh: Mesh.box(2, 2, 0.02), color: [1, 1, 1] });
        box.setShader({
            module: `
                module hook;
                import mesh;
                pub fn vertex(v: mesh::Vertex) -> mesh::Vertex {
                    var out = v;
                    out.v0.x = v.pos.y;
                    out.vi.x = 3;
                    return out;
                }
                pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
                    var out = s;
                    out.base_color = vec3<f32>(0.0);
                    out.metallic = 0.0;
                    out.roughness = 1.0;
                    out.emissive = vec3<f32>(step(0.0, v.v0.x), step(0.0, -v.v0.x), select(v.vi.x == 3, 1.0, 0.0));
                    return out;
                }`,
        });
        const img = s.scene.captureFrame();
        const top = rgbAt(img, 64, 40), bottom = rgbAt(img, 64, 88);
        assert(top[0] > 240 && top[1] < 10 && top[2] > 240, `the upper half reads v.v0.x > 0 ${fmt(top)}`);
        assert(bottom[0] < 10 && bottom[1] > 240 && bottom[2] > 240, `the lower half reads v.v0.x < 0 ${fmt(bottom)}`);
        dropScene(s);
    }

    // --- a vertex hook reading v.tangent still casts a shadow -----------------
    {
        const s = freshScene(S);
        const sc = s.scene;
        sc.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 6, 0.01], target: [0, 0, 0] });
        sc.setAmbient({ color: [0.2, 0.2, 0.2] });
        // A 45-degree sun throws a shadow 1.5 units along +x. The hook moves
        // the caster 2 units to -x, so its shadow falls left of centre; a
        // shadow pass that dropped the hook (falling back to the stock
        // caster) would shadow the right of centre instead.
        sc.createLight({ type: 'directional', direction: [1, -1, 0], intensity: 3, castShadow: true });
        sc.createMesh({ mesh: 'plane', halfW: 5, halfD: 5, color: [0.8, 0.8, 0.8] });
        const caster = sc.createMesh({ mesh: Mesh.box(1, 0.2, 1), y: 1.5, color: [1, 1, 1] });
        caster.setShader({
            module: `
                module hook;
                import mesh;
                pub fn vertex(v: mesh::Vertex) -> mesh::Vertex {
                    var out = v;
                    out.pos.x -= 2.0 + 0.0 * v.tangent.x;
                    return out;
                }`,
        });
        const img = sc.captureFrame();
        const moved = rgbAt(img, 55, 64), stock = rgbAt(img, 92, 64), open = rgbAt(img, 100, 100);
        assert(open[0] > 60, `the open floor is lit ${fmt(open)}`);
        assert(moved[0] < open[0] * 0.6,
               `the moved caster shadows the floor (${fmt(moved)} vs open floor ${fmt(open)})`);
        assert(stock[0] > open[0] * 0.9,
               `nothing shadows where the unmoved caster would (${fmt(stock)} vs ${fmt(open)})`);
        dropScene(s);
    }

    // --- the sampler budget ----------------------------------------------------
    {
        const s = freshScene(S);
        const box = s.scene.createMesh({ mesh: 'box' });
        const samplers = (n) => {
            let decls = '', sum = '0.0';
            for (let i = 0; i < n; i++) {
                decls += `@binding(4, ${i + 1}) var tex${i}: texture2d<vec4<f32>>;\n`;
                sum += ` + tex${i}.sample(custom::linear, v.uv).x`;
            }
            return emitting(decls, `vec3<f32>(${sum})`);
        };
        let threw = null;
        try { box.setShader({ module: samplers(8) }); } catch (e) { threw = e; }
        assert(threw === null, `eight samplers compile (${threw})`);
        threw = null;
        try { box.setShader({ module: samplers(9) }); } catch (e) { threw = e; }
        assert(threw !== null, 'a ninth sampler is refused');
        dropScene(s);
    }

    console.log('test_custom_shader_contract: OK');
}
