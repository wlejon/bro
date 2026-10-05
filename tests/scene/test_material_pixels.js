// Mesh materials on the GPU path, read back as pixels: each texture map and
// per-mesh switch changes what is drawn the way its doc says
// (scene-nodes-api.js MeshNodeOptions): normal, metal-roughness, emissive and
// occlusion maps; two-sided, near clip, depth bias, line drawing, vertex
// colour modes, subsurface and wind sway.

function patchColor(img, cx, cy, r) {
    const s = [0, 0, 0];
    let n = 0;
    for (let y = cy - r; y <= cy + r; y++) {
        for (let x = cx - r; x <= cx + r; x++) {
            if (x < 0 || y < 0 || x >= img.width || y >= img.height) continue;
            const i = (y * img.width + x) * 4;
            s[0] += img.data[i]; s[1] += img.data[i + 1]; s[2] += img.data[i + 2];
            n++;
        }
    }
    return n ? s.map((v) => v / n) : s;
}

function brightness(img, cx, cy, r) {
    const c = patchColor(img, cx, cy, r);
    return (c[0] + c[1] + c[2]) / 3;
}

function diffCount(a, b, tol) {
    let n = 0;
    for (let i = 0; i < a.data.length; i += 4) {
        if (Math.abs(a.data[i] - b.data[i]) > tol || Math.abs(a.data[i + 1] - b.data[i + 1]) > tol ||
            Math.abs(a.data[i + 2] - b.data[i + 2]) > tol) n++;
    }
    return n;
}

function freshScene(size) {
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

// A 1x1 (or w x h) RGBA8 texture of one colour.
function solid(r, g, b, a) {
    return { width: 1, height: 1, data: new Uint8Array([r, g, b, a === undefined ? 255 : a]) };
}

// An up-facing 2x2 m quad at the origin with +x tangents (so normal maps
// apply), uvs and optional per-vertex colours.
function quad(colors) {
    const g = {
        positions: new Float32Array([-1, 0, -1, -1, 0, 1, 1, 0, 1, 1, 0, -1]),
        normals: new Float32Array([0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0]),
        uvs: new Float32Array([0, 0, 0, 1, 1, 1, 1, 0]),
        tangents: new Float32Array([1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1]),
        indices: new Uint32Array([0, 1, 2, 0, 2, 3]),
    };
    if (colors) g.colors = new Float32Array(colors);
    return g;
}

// A scene looking straight down at the origin, with no ambient.
function topDown(size) {
    const s = freshScene(size);
    const scn = s.scene;
    scn.setCamera({ fov: 40, near: 0.1, far: 20, position: [0, 4, 0.001], target: [0, 0, 0] });
    scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scn.setAmbient([0, 0, 0]);
    return { s, scn };
}

const probe = freshScene(64);
if (!probe.scene) {
    missingGpuContext('scene');
} else {
    dropScene(probe);
    const C = 48;   // centre of a 96 px capture

    // ---- Normal map: tilting the shading normal toward / away from the sun.
    {
        const { s, scn } = topDown(96);
        scn.createLight({ type: 'directional', direction: [1, -1, 0], intensity: 3 });
        const node = scn.createMesh(Object.assign(quad(), { color: [0.8, 0.8, 0.8], roughness: 1 }));
        const plain = brightness(scn.captureFrame(), C, C, 3);
        node.destroy();
        // Tangent-space (-0.7, 0, 0.7): toward -x, i.e. into the light.
        const toward = scn.createMesh(Object.assign(quad(), { color: [0.8, 0.8, 0.8], roughness: 1,
                                                             normalTexture: solid(38, 128, 217) }));
        const lit = brightness(scn.captureFrame(), C, C, 3);
        toward.destroy();
        const away = scn.createMesh(Object.assign(quad(), { color: [0.8, 0.8, 0.8], roughness: 1,
                                                           normalTexture: solid(217, 128, 217) }));
        const dark = brightness(scn.captureFrame(), C, C, 3);
        away.destroy();
        const flat = scn.createMesh(Object.assign(quad(), { color: [0.8, 0.8, 0.8], roughness: 1,
                                                           normalTexture: solid(128, 128, 255) }));
        const same = brightness(scn.captureFrame(), C, C, 3);
        flat.destroy();
        assert(lit > plain * 1.2, `a normal map tilted into the light brightens (${lit.toFixed(1)} vs ${plain.toFixed(1)})`);
        assert(dark < plain * 0.3, `tilted away it darkens (${dark.toFixed(1)} vs ${plain.toFixed(1)})`);
        assert(Math.abs(same - plain) < 3, `a flat normal map changes nothing (${same.toFixed(1)} vs ${plain.toFixed(1)})`);
        dropScene(s);
    }

    // ---- Metal-roughness, emissive and occlusion maps ----------------------
    {
        const { s, scn } = topDown(96);
        scn.createLight({ type: 'directional', direction: [0, -1, 0], intensity: 3 });
        const shot = (opts) => {
            const n = scn.createMesh(Object.assign(quad(), opts));
            const b = brightness(scn.captureFrame(), C, C, 3);
            n.destroy();
            return b;
        };
        // glTF packing: G = roughness, B = metallic (multiplying the factors).
        const metal = shot({ color: [0.8, 0.8, 0.8], metallic: 1, roughness: 1 });
        const mrDielectric = shot({ color: [0.8, 0.8, 0.8], metallic: 1, roughness: 1,
                                    metallicRoughnessTexture: solid(0, 255, 0) });
        assert(mrDielectric > metal * 2,
            `an MR map with B = 0 makes the metal diffuse (${mrDielectric.toFixed(1)} vs ${metal.toFixed(1)})`);

        const glow = shot({ color: [0, 0, 0], emissive: 1, emissiveColor: [0, 1, 0] });
        const glowMasked = shot({ color: [0, 0, 0], emissive: 1, emissiveColor: [0, 1, 0],
                                  emissiveTexture: solid(0, 0, 0) });
        assert(glow > 60 && glowMasked < glow * 0.2,
            `a black emissive map masks the glow (${glowMasked.toFixed(1)} vs ${glow.toFixed(1)})`);

        // Unlit is the base colour as authored: an emissive term adds nothing.
        const unlitShot = (opts) => {
            const n = scn.createMesh(Object.assign(quad(), opts));
            const c = patchColor(scn.captureFrame(), C, C, 3);
            n.destroy();
            return c;
        };
        const unlit = unlitShot({ color: [0.5, 0.2, 0.2], unlit: true });
        const unlitGlow = unlitShot({ color: [0.5, 0.2, 0.2], unlit: true, emissive: 2, emissiveColor: [0, 1, 0] });
        assert(unlit[0] > 100 && unlit[1] < 70,
            `an unlit mesh draws its base colour (rgb ${unlit.map((v) => v.toFixed(0))})`);
        assert(Math.abs(unlitGlow[0] - unlit[0]) < 3 && Math.abs(unlitGlow[1] - unlit[1]) < 3,
            `emissive does not add to an unlit mesh (rgb ${unlitGlow.map((v) => v.toFixed(0))} vs ` +
            `${unlit.map((v) => v.toFixed(0))})`);

        // Occlusion darkens only the ambient (the AO map's R channel).
        scn.setAmbient([0.6, 0.6, 0.6]);
        const ambient = shot({ color: [0.3, 0.3, 0.3], roughness: 1 });
        const occluded = shot({ color: [0.3, 0.3, 0.3], roughness: 1, occlusionTexture: solid(64, 0, 0) });
        assert(occluded < ambient * 0.8,
            `an occlusion map darkens the ambient (${occluded.toFixed(1)} vs ${ambient.toFixed(1)})`);
        dropScene(s);
    }

    // ---- Two-sided, subsurface, near clip ----------------------------------
    {
        const s = freshScene(96);
        const scn = s.scene;
        // Under the quad, looking up at its back.
        scn.setCamera({ fov: 40, near: 0.1, far: 20, position: [0, -4, 0.001], target: [0, 0, 0] });
        scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
        scn.setAmbient([0, 0, 0]);
        scn.createLight({ type: 'directional', direction: [0, 1, 0], intensity: 3 });   // from below
        const shot = (opts) => {
            const n = scn.createMesh(Object.assign(quad(), { color: [0.8, 0.8, 0.8], roughness: 1 }, opts));
            const b = brightness(scn.captureFrame(), C, C, 3);
            n.destroy();
            return b;
        };
        const culled = shot({});
        const twoSided = shot({ twoSided: true });
        assert(culled < 5, `a one-sided back face is culled (${culled.toFixed(1)})`);
        assert(twoSided > 60, `a two-sided back face draws, lit from its side (${twoSided.toFixed(1)})`);
        dropScene(s);

        // Light the front only: the back is dark unless subsurface wraps it.

        const t = freshScene(96);
        const scn2 = t.scene;
        scn2.setCamera({ fov: 40, near: 0.1, far: 20, position: [0, -4, 0.001], target: [0, 0, 0] });
        scn2.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
        scn2.setAmbient([0, 0, 0]);
        scn2.createLight({ type: 'directional', direction: [0, -1, 0], intensity: 3 });  // from above
        const shot2 = (opts) => {
            const n = scn2.createMesh(Object.assign(quad(), { color: [0.8, 0.8, 0.8], roughness: 1 }, opts));
            const b = brightness(scn2.captureFrame(), C, C, 3);
            n.destroy();
            return b;
        };
        const back = shot2({ twoSided: true });
        const wrapped = shot2({ twoSided: true, subsurface: 0.8 });
        assert(back < 5 && wrapped > back + 20,
            `subsurface lets the light through a thin leaf (${wrapped.toFixed(1)} vs ${back.toFixed(1)})`);
        // Near clip: fragments closer to the eye than nearClipDist are cut.
        const kept = shot2({ twoSided: true, subsurface: 0.8, nearClipDist: 1 });
        const clipped = shot2({ twoSided: true, subsurface: 0.8, nearClipDist: 5 });
        assert(Math.abs(kept - wrapped) < 3 && clipped < 5,
            `nearClipDist discards the near surface (${clipped.toFixed(1)}, kept ${kept.toFixed(1)})`);
        dropScene(t);
    }

    // ---- Depth bias: a coplanar decal-style quad wins with a negative bias --
    {
        const { s, scn } = topDown(96);
        scn.createLight({ type: 'directional', direction: [0, -1, 0], intensity: 3 });
        scn.createMesh(Object.assign(quad(), { color: [1, 0, 0], roughness: 1 }));
        const over = scn.createMesh(Object.assign(quad(), { color: [0, 0, 1], roughness: 1, depthBias: [-1, -4] }));
        let c = patchColor(scn.captureFrame(), C, C, 3);
        assert(c[2] > c[0] * 3, `a negative depth bias draws on top (rgb ${c.map((v) => v.toFixed(0))})`);
        over.destroy();
        scn.createMesh(Object.assign(quad(), { color: [0, 0, 1], roughness: 1, depthBias: [1, 4] }));
        c = patchColor(scn.captureFrame(), C, C, 3);
        assert(c[0] > c[2] * 3, `a positive depth bias draws behind (rgb ${c.map((v) => v.toFixed(0))})`);
        dropScene(s);
    }

    // ---- Lines: the edges, not the faces -----------------------------------
    {
        const { s, scn } = topDown(96);
        scn.createLight({ type: 'directional', direction: [0, -1, 0], intensity: 3 });
        const geo = quad();
        // Line list along the quad's four edges.
        geo.indices = new Uint32Array([0, 1, 1, 2, 2, 3, 3, 0]);
        scn.createMesh(Object.assign(geo, { color: [1, 1, 1], unlit: true, drawMode: 'lines' }));
        const img = scn.captureFrame();
        const centre = brightness(img, C, C, 3);
        const edge = scn.projectLocal(1, 0, 0);
        let best = 0;
        for (let dx = -2; dx <= 2; dx++) {
            best = Math.max(best, brightness(img, Math.round(edge.x) + dx, Math.round(edge.y), 0));
        }
        assert(centre < 5, `the quad's interior is empty in line mode (${centre.toFixed(1)})`);
        assert(best > 150, `its edge is drawn (${best.toFixed(1)})`);
        dropScene(s);
    }

    // ---- Vertex colours: replace by default, tint, or ignored --------------
    {
        const { s, scn } = topDown(96);
        scn.createLight({ type: 'directional', direction: [0, -1, 0], intensity: 3 });
        const green = [0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1];
        const shot = (opts) => {
            const n = scn.createMesh(Object.assign(quad(green), { roughness: 1 }, opts));
            const c = patchColor(scn.captureFrame(), C, C, 3);
            n.destroy();
            return c;
        };
        const replaced = shot({ color: [1, 0, 0] });
        assert(replaced[1] > 60 && replaced[0] < 5, `vertex colours replace the albedo (rgb ${replaced})`);
        const tinted = shot({ color: [1, 1, 0], vertexColorTint: true });
        assert(tinted[1] > 60 && tinted[0] < 5, `tint multiplies (yellow x green = green, rgb ${tinted})`);
        const tintedRed = shot({ color: [1, 0, 0], vertexColorTint: true });
        assert(tintedRed[0] < 5 && tintedRed[1] < 5, `tint multiplies (red x green = black, rgb ${tintedRed})`);
        const ignored = shot({ color: [1, 0, 0], vertexColorTint: false });
        assert(ignored[0] > 60 && ignored[1] < 5, `vertexColorTint false ignores them (rgb ${ignored})`);
        dropScene(s);
    }

    // ---- Wind: swaying meshes move over time; others stay put --------------
    {
        const s = freshScene(96);
        const scn = s.scene;
        scn.setCamera({ fov: 40, near: 0.1, far: 30, position: [0, 1, 6], target: [0, 1, 0] });
        scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
        scn.createLight({ type: 'directional', direction: [0, -0.5, -1], intensity: 3 });
        // A tall thin blade whose top bends (vertex colour R is the bend).
        const blade = {
            positions: new Float32Array([-0.1, 0, 0, 0.1, 0, 0, 0.1, 2, 0, -0.1, 2, 0]),
            normals: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1]),
            colors: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1]),
            indices: new Uint32Array([0, 1, 2, 0, 2, 3]),
        };
        scn.setWind({ direction: [1, 0, 0], strength: 0.6, frequency: 3 });
        const still = scn.createMesh(Object.assign({}, blade, { color: [1, 1, 1], vertexColorTint: false }));
        const a = scn.captureFrame();
        advanceTime(400);
        const b = scn.captureFrame();
        assert(diffCount(a, b, 4) === 0, 'a mesh without `wind` does not sway');
        still.destroy();
        scn.createMesh(Object.assign({}, blade, { color: [1, 1, 1], vertexColorTint: false, wind: 1 }));
        const c = scn.captureFrame();
        advanceTime(400);
        const d = scn.captureFrame();
        assert(diffCount(c, d, 4) > 10, `a wind mesh sways between frames (${diffCount(c, d, 4)} px)`);
        dropScene(s);
    }
}
