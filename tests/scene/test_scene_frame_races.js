// 3D scene renders on the shared frame core: every render takes its camera,
// lighting, per-node uniforms, instance data and bone palettes from the
// frame's own memory, so state changed between consecutive renders — with
// earlier renders still in flight — reaches exactly the render it was set
// for. And the tonemapped result is read back only on demand: through the
// compositor (scene smaller than the frame: CPU readback, predicted into the
// render after the first read), through captureFrame, and as the GPU image
// the presenter samples (scene covering the frame), every one of them showing
// the frame it was asked for, never the one before.
// Exercises src/scene/vulkan (bridge frame resources, readback, upload stream).

const palette = [[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 0],
                 [255, 0, 255], [0, 255, 255], [255, 255, 255], [255, 128, 0]];
const hex = (c) => '#' + c.map(v => v.toString(16).padStart(2, '0')).join('');

function dominant(p, c, msg) {
    // Unlit + linear tonemap reproduces the colour closely; allow drift from
    // the colour pipeline but demand each channel lands on the right side.
    for (let k = 0; k < 3; ++k) {
        if (c[k] >= 200) assert(p[k] > 150, `${msg}: channel ${k} high (got ${Array.from(p).slice(0, 3)})`);
        else if (c[k] === 0) assert(p[k] < 60, `${msg}: channel ${k} low (got ${Array.from(p).slice(0, 3)})`);
    }
}

function region(img, x0, y0, x1, y1) {
    const sum = [0, 0, 0];
    let n = 0;
    for (let y = y0; y < y1; y++) {
        for (let x = x0; x < x1; x++) {
            const i = (y * img.width + x) * 4;
            sum[0] += img.data[i]; sum[1] += img.data[i + 1]; sum[2] += img.data[i + 2];
            n++;
        }
    }
    return sum.map(v => v / n);
}

function freshScene(w, h, style) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(w));
    cv.setAttribute('height', String(h));
    if (style) cv.setAttribute('style', style);
    document.body.appendChild(cv);
    flush();
    const sc = cv.getContext('scene');
    if (sc) {
        sc.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 4], target: [0, 0, 0] });
        sc.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    }
    return { canvas: cv, scene: sc };
}

function dropScene(s) {
    document.body.removeChild(s.canvas);
    flush();
}

const emissive = `
    uniform vec3 u_color;
    void userFragment(inout vec3 baseColor, inout vec3 normal,
                      inout float metallic, inout float roughness,
                      inout vec3 emissive, inout float alpha) {
        baseColor = vec3(0.0);
        metallic = 0.0;
        roughness = 1.0;
        emissive = u_color;
    }`;

document.body.setAttribute('style', 'margin:0; background:#000');
const probe = freshScene(32, 32);
if (!probe.scene) {
    console.log('scene context not available (no GPU) — skipping frame race test');
} else {
    dropScene(probe);

    // =====================================================================
    // Compositor readback of a scene smaller than the frame, one colour per
    // frame: the first read copies on demand, later ones are recorded into
    // the render itself — every frame must show its own colour.
    // =====================================================================
    {
        const s = freshScene(64, 64, 'position:absolute; left:0; top:0; width:64px; height:64px');
        const m = s.scene.createMesh({ mesh: 'box', color: '#ffffff', scale: [3, 3, 3] });
        m.setShader({ fragment: emissive, uniforms: { u_color: [1, 0, 0] } });
        for (let i = 0; i < palette.length; ++i) {
            const c = palette[i];
            m.setShaderUniform('u_color', c.map(v => v / 255));
            advanceTime(16);
            const px = getPixels(32, 32, 1, 1).data;
            dominant(px, c, `composited scene, frame ${i}`);
        }
        dropScene(s);
    }

    // =====================================================================
    // captureFrame renders and reads back synchronously; consecutive
    // captures with no frame in between still each see their own state.
    // =====================================================================
    {
        const s = freshScene(64, 64);
        const m = s.scene.createMesh({ mesh: 'box', color: '#ffffff', scale: [3, 3, 3] });
        m.setShader({ fragment: emissive, uniforms: { u_color: [0, 0, 0] } });
        for (let i = 0; i < palette.length; ++i) {
            const c = palette[i];
            m.setShaderUniform('u_color', c.map(v => v / 255));
            const img = s.scene.captureFrame();
            dominant(region(img, 28, 28, 36, 36), c, `captureFrame ${i}`);
        }
        dropScene(s);
    }

    // =====================================================================
    // Many nodes, each with its own uniform values, in one render.
    // =====================================================================
    {
        const s = freshScene(256, 32);
        s.scene.setCamera({ fov: 18, near: 0.1, far: 100, position: [0, 0, 4], target: [0, 0, 0] });
        const nodes = palette.map((c, i) => {
            const m = s.scene.createMesh({ mesh: 'box', color: '#ffffff', position: [-3.5 + i, 0, 0],
                                           scale: [0.9, 0.9, 0.9] });
            m.setShader({ fragment: emissive, uniforms: { u_color: c.map(v => v / 255) } });
            return m;
        });
        const img = s.scene.captureFrame();
        // Probe each box's centre where the camera projects it.
        let probed = 0;
        for (let i = 0; i < nodes.length; ++i) {
            const p = s.scene.projectLocal(-3.5 + i, 0, 0);
            assert(p && !p.behind, `node ${i} projects onto the canvas`);
            const x = Math.round(p.x), y = Math.round(p.y);
            if (x < 3 || x > 252 || y < 3 || y > 28) continue;
            dominant(region(img, x - 2, y - 2, x + 3, y + 3), palette[i], `node ${i} uniforms`);
            probed++;
        }
        assert(probed === nodes.length, `every node lands on the canvas (${probed})`);
        dropScene(s);
    }

    // =====================================================================
    // Instance data rewritten every frame: each frame shows that frame's
    // instance tint and position, through the compositor.
    // =====================================================================
    {
        const s = freshScene(64, 64, 'position:absolute; left:0; top:0; width:64px; height:64px');
        const inst = (tx, c) => [1.5, 0, 0, tx, 0, 1.5, 0, 0, 0, 0, 1.5, 0, c[0] / 255, c[1] / 255, c[2] / 255, 1];
        const im = s.scene.createInstancedMesh({ mesh: Mesh.box(), color: '#ffffff', unlit: true,
                                                 instances: new Float32Array(inst(-1, palette[0])) });
        for (let i = 0; i < 6; ++i) {
            const c = palette[i];
            const left = i % 2 === 0;
            im.setInstances(new Float32Array(inst(left ? -1 : 1, c)));
            advanceTime(16);
            const lit = getPixels(left ? 16 : 48, 32, 1, 1).data;
            const dark = getPixels(left ? 48 : 16, 32, 1, 1).data;
            dominant(lit, c, `instance frame ${i}`);
            assert(dark[0] + dark[1] + dark[2] < 90,
                   `instance frame ${i}: the previous frame's position is empty (got ${Array.from(dark).slice(0, 3)})`);
        }
        dropScene(s);
    }

    // =====================================================================
    // A scene covering the whole frame is presented from its GPU image —
    // no CPU readback — and still shows each frame's colour.
    // =====================================================================
    {
        resize(64, 64);
        const s = freshScene(64, 64, 'position:absolute; left:0; top:0; width:64px; height:64px');
        const m = s.scene.createMesh({ mesh: 'box', color: '#ffffff', scale: [3, 3, 3] });
        m.setShader({ fragment: emissive, uniforms: { u_color: [0, 0, 0] } });
        for (let i = 0; i < palette.length; ++i) {
            const c = palette[i];
            m.setShaderUniform('u_color', c.map(v => v / 255));
            advanceTime(16);
            dominant(getPixels(32, 32, 1, 1).data, c, `full-frame scene, frame ${i}`);
        }
        dropScene(s);
    }
}
