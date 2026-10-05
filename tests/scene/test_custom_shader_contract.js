// The custom-shader chunk contract, by pixels: chunks are written against
// the GL-era names and declarations, and the Vulkan renderer rewrites them.
//   - the engine varyings vUV, vWorldPos (camera-relative), vCamDist and the
//     vertex inputs (aTangent among them) answer by their GL names;
//   - a u_* uniform used in both stages reads the same value in each, and a
//     line may declare several;
//   - a varying the vertex chunk writes reaches the fragment chunk;
//   - a chunk that reads aTangent still casts a shadow (the shadow pass
//     compiles the vertex chunk too);
//   - eight samplers fit, a ninth is refused with a compile error.

function freshScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    const sc = cv.getContext('scene');
    if (sc) {
        sc.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 4], target: [0, 0, 0] });
        sc.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
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

// Emit `expr` (a vec3) and nothing else.
const emitting = (decls, expr) => `${decls}
    void userFragment(inout vec3 baseColor, inout vec3 normal,
                      inout float metallic, inout float roughness,
                      inout vec3 emissive, inout float alpha) {
        baseColor = vec3(0.0);
        metallic = 0.0;
        roughness = 1.0;
        emissive = ${expr};
    }`;

const probe = freshScene(32);
if (!probe.scene) {
    missingGpuContext('scene');
} else {
    dropScene(probe);
    const S = 128;

    // --- GL names for the engine varyings -------------------------------------
    {
        const s = freshScene(S);
        const box = s.scene.createMesh({ mesh: Mesh.box(2, 2, 0.02), color: [1, 1, 1] });
        box.setShader({ fragment: emitting('', 'vec3(vUV, clamp(-vWorldPos.z / 8.0, 0.0, 1.0)) * step(3.0, vCamDist)') });
        const c = rgbAt(s.scene.captureFrame(), 64, 64);
        assert(near(c[0], 128, 12) && near(c[1], 128, 12), `vUV is the face's uv at its centre ${fmt(c)}`);
        assert(near(c[2], 126, 12), `vWorldPos is camera-relative and vCamDist the eye distance ${fmt(c)}`);
        dropScene(s);
    }

    // --- one uniform block for both stages; several to a line -----------------
    {
        const s = freshScene(S);
        const box = s.scene.createMesh({ mesh: Mesh.box(0.6, 0.6, 0.02), color: [1, 1, 1] });
        box.setShader({
            vertex: `uniform float u_shift, u_unused;
                uniform vec3 u_color;
                void userVertex(inout vec3 pos, inout vec3 normal, inout vec2 uv) {
                    pos.x += u_shift;
                }`,
            fragment: emitting('uniform vec3 u_color;\nuniform float u_shift;', 'u_color * step(0.5, u_shift)'),
            uniforms: { u_shift: 1.0, u_unused: 7.0, u_color: [0.25, 0.5, 1.0] },
        });
        const img = s.scene.captureFrame();
        const moved = rgbAt(img, 64 + 32, 64);
        const left = rgbAt(img, 64, 64);
        assert(near(moved[0], 64, 6) && near(moved[1], 128, 8) && near(moved[2], 255, 8),
               `both stages read u_shift and u_color from one block ${fmt(moved)}`);
        assert(left[0] + left[1] + left[2] < 10, `the vertex stage moved the box off centre ${fmt(left)}`);
        dropScene(s);
    }

    // --- a custom varying -------------------------------------------------------
    {
        const s = freshScene(S);
        const box = s.scene.createMesh({ mesh: Mesh.box(2, 2, 0.02), color: [1, 1, 1] });
        box.setShader({
            vertex: `out float v_side;
                flat out int v_tag;
                void userVertex(inout vec3 pos, inout vec3 normal, inout vec2 uv) {
                    v_side = pos.y;
                    v_tag = 3;
                }`,
            fragment: emitting('in float v_side;\nflat in int v_tag;',
                               'vec3(step(0.0, v_side), step(0.0, -v_side), v_tag == 3 ? 1.0 : 0.0)'),
        });
        const img = s.scene.captureFrame();
        const top = rgbAt(img, 64, 40), bottom = rgbAt(img, 64, 88);
        assert(top[0] > 240 && top[1] < 10 && top[2] > 240, `the upper half reads v_side > 0 ${fmt(top)}`);
        assert(bottom[0] < 10 && bottom[1] > 240 && bottom[2] > 240, `the lower half reads v_side < 0 ${fmt(bottom)}`);
        dropScene(s);
    }

    // --- a vertex chunk reading aTangent still casts a shadow -------------------
    {
        const s = freshScene(S);
        const sc = s.scene;
        sc.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 6, 0.01], target: [0, 0, 0] });
        sc.setAmbient({ color: [0.2, 0.2, 0.2] });
        // A 45-degree sun throws a shadow 1.5 units along +x. The chunk moves
        // the caster 2 units to -x, so its shadow falls left of centre; a
        // shadow pass that dropped the chunk (falling back to the stock
        // caster) would shadow the right of centre instead.
        sc.createLight({ type: 'directional', direction: [1, -1, 0], intensity: 3, castShadow: true });
        sc.createMesh({ mesh: 'plane', halfW: 5, halfD: 5, color: [0.8, 0.8, 0.8] });
        const caster = sc.createMesh({ mesh: Mesh.box(1, 0.2, 1), y: 1.5, color: [1, 1, 1] });
        caster.setShader({
            vertex: `void userVertex(inout vec3 pos, inout vec3 normal, inout vec2 uv) {
                    pos.x -= 2.0 + 0.0 * aTangent.x;
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

    // --- the sampler budget ------------------------------------------------------
    {
        const s = freshScene(S);
        const box = s.scene.createMesh({ mesh: 'box' });
        const samplers = (n) => {
            let decls = '', sum = '0.0';
            for (let i = 0; i < n; i++) {
                decls += `uniform sampler2D u_tex${i};\n`;
                sum += ` + texture(u_tex${i}, vUV).r`;
            }
            return emitting(decls, `vec3(${sum})`);
        };
        let threw = null;
        try { box.setShader({ fragment: samplers(8) }); } catch (e) { threw = e; }
        assert(threw === null, `eight samplers compile (${threw})`);
        threw = null;
        try { box.setShader({ fragment: samplers(9) }); } catch (e) { threw = e; }
        assert(threw !== null, 'a ninth sampler is refused');
        dropScene(s);
    }

    console.log('test_custom_shader_contract: OK');
}
