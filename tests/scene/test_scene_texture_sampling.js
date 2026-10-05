// How node images are sampled, by their pixels (GL parity):
//   - a sprite is pixel art: nearest filtering, so a magnified two-texel
//     sheet keeps a hard edge between its texels;
//   - a sprite clamps rather than repeats, so the texel at one edge does not
//     bleed in from the other.

const os = require('os');
const path = require('path');
const fs = require('fs');

const tmpDir = path.join(os.tmpdir(), 'bro_test_tex_sampling_' + Date.now());
fs.mkdirSync(tmpDir, { recursive: true });

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
    }
    return { canvas: cv, scene: sc };
}

const probe = freshScene(32);
if (!probe.scene) {
    missingGpuContext('scene');
} else {
    document.body.removeChild(probe.canvas);
    const SIZE = 128;

    // Two texels: red | blue.
    const sheet = path.join(tmpDir, 'red_blue.png');
    assert(bro.image.encodePngFile(sheet, new Uint8Array([255, 0, 0, 255, 0, 0, 255, 255]), 2, 1, 4),
           'the two-texel sheet is written');

    const s = freshScene(SIZE);
    s.scene.createSprite({ src: sheet, width: 3, height: 1, worldAnchor: [0, 0, 0] });
    advanceTime(16);
    const img = s.scene.captureFrame();
    const at = (x, y) => Array.from(img.data.slice((y * SIZE + x) * 4, (y * SIZE + x) * 4 + 4));
    const fmt = (p) => '(' + p.join(',') + ')';

    // The sprite spans about 64 px across the centre; its texel seam is at
    // x = 64. A few pixels either side of it nearest filtering keeps each
    // texel pure, where linear filtering would mix them.
    const left = at(58, 64), right = at(70, 64);
    assert(left[3] > 200 && left[0] > 200 && left[2] < 10, `left of the seam is pure red ${fmt(left)}`);
    assert(right[3] > 200 && right[2] > 200 && right[0] < 10, `right of the seam is pure blue ${fmt(right)}`);

    // At the sprite's left edge a repeating sampler would pull in blue.
    const edge = at(35, 64);
    assert(edge[3] > 200 && edge[2] < 10, `the left edge clamps to red ${fmt(edge)}`);

    document.body.removeChild(s.canvas);
    fs.rmSync(tmpDir, { recursive: true, force: true });
    console.log('test_scene_texture_sampling: OK');
}
