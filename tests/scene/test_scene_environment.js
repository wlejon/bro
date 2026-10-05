// The scene's environment and sky, by their pixels:
//   - an HDR panorama (setEnvironment) lights meshes through its irradiance
//     and draws as the sky; rotation turns both, intensity scales both, and
//     clearing it brings back the transparent background;
//   - the physical atmosphere (setAtmosphere) draws a blue day sky;
//   - the starfield (setStarfield) draws stars into the sky.
// The panorama is written here as a Radiance .hdr: four quadrants of
// longitude, red / green / blue / black, so any face of a white box picks up
// a colour from the quarter of the world it faces.

const fs = require('fs');
const os = require('os');
const path = require('path');
const tmpDir = path.join(os.tmpdir(), 'bro_test_env_' + Date.now());
fs.mkdirSync(tmpDir, { recursive: true });

// Flat (non-RLE) RGBE scanlines.
function writeHdr(file, w, h, colorAt) {
    const header = `#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y ${h} +X ${w}\n`;
    const bytes = new Uint8Array(header.length + w * h * 4);
    for (let i = 0; i < header.length; i++) bytes[i] = header.charCodeAt(i);
    let o = header.length;
    for (let y = 0; y < h; y++) {
        for (let x = 0; x < w; x++) {
            const c = colorAt(x, y);
            const m = Math.max(c[0], c[1], c[2]);
            if (m < 1e-32) { o += 4; continue; }
            const e = Math.floor(Math.log2(m)) + 1;   // m / 2^e in [0.5, 1)
            const scale = 256 / Math.pow(2, e);
            bytes[o++] = Math.min(255, Math.floor(c[0] * scale));
            bytes[o++] = Math.min(255, Math.floor(c[1] * scale));
            bytes[o++] = Math.min(255, Math.floor(c[2] * scale));
            bytes[o++] = e + 128;
        }
    }
    fs.writeFileSync(file, bytes);
}

const QUADRANTS = [[1, 0, 0], [0, 1, 0], [0, 0, 1], [0, 0, 0]];
const hdrPath = path.join(tmpDir, 'quadrants.hdr');
writeHdr(hdrPath, 32, 16, (x) => QUADRANTS[Math.floor(x / 8)]);

function freshScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    const sc = cv.getContext('scene');
    if (sc) sc.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    return { canvas: cv, scene: sc };
}

function dropScene(s) {
    document.body.removeChild(s.canvas);
    flush();
}

// Mean RGBA over a box.
function boxRGBA(img, x0, y0, x1, y1) {
    const sum = [0, 0, 0, 0];
    let n = 0;
    for (let y = y0; y < y1; y++) {
        for (let x = x0; x < x1; x++) {
            const i = (y * img.width + x) * 4;
            for (let c = 0; c < 4; c++) sum[c] += img.data[i + c];
            n++;
        }
    }
    return sum.map((v) => v / n);
}

const fmt = (c) => '(' + c.map((v) => v.toFixed(0)).join(',') + ')';
const chroma = (c) => Math.max(c[0], c[1], c[2]) - Math.min(c[0], c[1], c[2]);
const rgbDiff = (a, b) => Math.abs(a[0] - b[0]) + Math.abs(a[1] - b[1]) + Math.abs(a[2] - b[2]);

const probe = freshScene(32);
if (!probe.scene) {
    missingGpuContext('scene');
} else {
    dropScene(probe);
    const SIZE = 128;

    // =====================================================================
    // Panorama IBL + skybox.
    // =====================================================================
    {
        const s = freshScene(SIZE);
        const sc = s.scene;
        sc.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 5], target: [0, 0, 0] });
        sc.createLight({ type: 'directional', intensity: 0 });   // no sun: only the environment lights
        sc.setAmbient({ color: [0, 0, 0] });
        sc.createMesh({ mesh: Mesh.box(1.5, 1.5, 0.2), color: [1, 1, 1, 1], roughness: 1, metallic: 0 });

        const face = (img) => boxRGBA(img, 56, 56, 72, 72);
        const sky = (img) => boxRGBA(img, 2, 2, 18, 18);

        const before = sc.captureFrame();
        assert(sky(before)[3] < 5, `no environment: the background is clear ${fmt(sky(before))}`);

        assert(sc.setEnvironment({ panorama: hdrPath }) === true, 'the panorama loads');
        const lit = sc.captureFrame();
        const f0 = face(lit), s0 = sky(lit);
        assert(chroma(f0) > 25, `the panorama tints the box's face ${fmt(f0)} (was ${fmt(face(before))})`);
        assert(s0[3] > 250 && chroma(s0) > 60, `the panorama draws as the sky ${fmt(s0)}`);

        sc.setEnvironment({ rotation: Math.PI / 2 });
        const turned = sc.captureFrame();
        const f1 = face(turned), s1 = sky(turned);
        assert(rgbDiff(f0, f1) > 40, `rotation turns the lighting: ${fmt(f0)} -> ${fmt(f1)}`);
        assert(rgbDiff(s0, s1) > 60, `rotation turns the sky: ${fmt(s0)} -> ${fmt(s1)}`);

        sc.setEnvironment({ intensity: 2 });
        const bright = sc.captureFrame();
        const f2 = face(bright);
        const sum = (c) => c[0] + c[1] + c[2];
        assert(sum(f2) > sum(f1) * 1.5, `intensity scales the lighting: ${fmt(f1)} -> ${fmt(f2)}`);

        sc.setEnvironment(null);
        const cleared = sc.captureFrame();
        assert(sky(cleared)[3] < 5, `clearing the environment clears the sky ${fmt(sky(cleared))}`);
        dropScene(s);
    }

    // =====================================================================
    // Atmosphere: a day sky above the horizon is blue.
    // =====================================================================
    {
        const s = freshScene(SIZE);
        const sc = s.scene;
        sc.setCamera({ fov: 60, near: 0.1, far: 1000, position: [0, 2, 0], target: [0, 4, -5] });
        sc.setToneMap({ mode: 'aces', exposure: 1.0 });
        sc.setAtmosphere({ sunPosition: [0, 0.6, 0.8] });
        const img = sc.captureFrame();
        const c = boxRGBA(img, 40, 20, 88, 60);
        assert(c[3] > 250, `the atmosphere fills the background ${fmt(c)}`);
        assert(c[2] > c[0] + 20 && c[2] > 60, `the day sky is blue ${fmt(c)}`);
        dropScene(s);
    }

    // =====================================================================
    // Starfield: points of light across an otherwise black sky.
    // =====================================================================
    {
        const s = freshScene(SIZE);
        const sc = s.scene;
        sc.setCamera({ fov: 60, near: 0.1, far: 1000, position: [0, 0, 0], target: [0, 5, -1] });
        const dark = sc.captureFrame();
        sc.setStarfield({ starCount: 2000, starSize: 4 });
        const img = sc.captureFrame();
        const stars = (im) => {
            let n = 0;
            for (let i = 0; i < im.data.length; i += 4) {
                if (Math.max(im.data[i], im.data[i + 1], im.data[i + 2]) > 60) n++;
            }
            return n;
        };
        assert(stars(dark) === 0, 'no starfield: no stars');
        const n = stars(img);
        assert(n > 5 && n < SIZE * SIZE / 4, `the starfield draws scattered stars: ${n} bright pixels`);
        dropScene(s);
    }

    console.log('test_scene_environment: OK');
}
