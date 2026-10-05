// The windowed presentation path, end to end: a real (SDL offscreen) window,
// its VulkanSwapchain and the VulkanPresenter compositing every layer kind,
// plus a bro.window.open() window on its own swapchain. presentedFrame() is
// the swapchain image as presented (BRO_CAPTURE_PRESENTS=1, set by
// tests/run_tests.sh), so these are the pixels a user would see.

function px(img, x, y) {
    const i = (y * img.width + x) * 4;
    return [img.data[i], img.data[i + 1], img.data[i + 2], img.data[i + 3]];
}
function near(img, x, y, rgb, tol, what) {
    const p = px(img, x, y);
    const ok = Math.abs(p[0] - rgb[0]) <= tol && Math.abs(p[1] - rgb[1]) <= tol && Math.abs(p[2] - rgb[2]) <= tol;
    assert(ok, `${what} at (${x},${y}): expected ~(${rgb}), got (${p})`);
}
function frames(n) {
    return new Promise((resolve) => {
        const step = () => (--n <= 0 ? resolve() : requestAnimationFrame(step));
        requestAnimationFrame(step);
    });
}

const c2d = document.getElementById('c2d').getContext('2d');
c2d.fillStyle = '#ff0000';
c2d.fillRect(0, 0, 40, 40);
const scaled = document.getElementById('scaled').getContext('2d');
scaled.fillStyle = '#ff00ff';
scaled.fillRect(0, 0, 20, 20);

const gl = document.getElementById('gl').getContext('webgl2');
function drawGL() {
    gl.clearColor(0, 0, 1, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    requestAnimationFrame(drawGL);
}

const scene = document.getElementById('scene').getContext('scene');

async function run() {
    if (!gl || !scene) {
        missingGpuContext(gl ? 'scene' : 'webgl2');
        return;
    }
    requestAnimationFrame(drawGL);
    scene.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scene.createLight({ type: 'directional', direction: [0, 0, -1], color: [1, 1, 1], intensity: 2 });
    scene.createMesh({ mesh: 'box', color: 'red', x: 0, y: 0, z: 0 });
    scene.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 3], target: [0, 0, 0] });

    await frames(20);
    const img = presentedFrame();
    if (!img) {
        assert(false, 'presentedFrame() returned nothing (BRO_CAPTURE_PRESENTS=1 and a readable swapchain needed)');
        return;
    }
    assert(img.width === 320 && img.height === 240, `presented frame is the window, got ${img.width}x${img.height}`);
    near(img, 20, 20, [0, 255, 0], 2, 'HTML');
    near(img, 300, 220, [0x20, 0x30, 0x40], 2, 'page background');
    near(img, 80, 30, [255, 0, 0], 2, '2D canvas layer');
    near(img, 150, 40, [0, 0, 255], 2, 'WebGL layer');
    near(img, 170, 40, [0x20, 0x30, 0x40], 2, 'WebGL cut to its overflow clip');
    near(img, 125, 15, [128, 128, 255], 3, 'translucent HTML blended over WebGL');
    const s = px(img, 230, 40);
    assert(s[0] > 100 && s[0] > s[1] + s[2], `3D scene layer: a lit red box, got (${s})`);
    near(img, 60, 150, [0x22, 0x44, 0xaa], 2, 'iframe layer');
    near(img, 235, 155, [255, 0, 255], 2, 'canvas under a scale(2) ancestor');
    near(img, 245, 155, [0x20, 0x30, 0x40], 2, 'past the scaled canvas');

    const win = bro.window.open('second', { width: 120, height: 80, title: 'second' });
    await frames(20);
    const second = presentedFrame(win);
    if (!second) {
        assert(false, 'nothing presented to the secondary window');
        return;
    }
    assert(second.width === 120 && second.height === 80,
           `secondary frame is its window, got ${second.width}x${second.height}`);
    near(second, 25, 25, [0, 255, 0], 2, 'secondary window 2D canvas');
    near(second, 90, 60, [0xaa, 0x44, 0x22], 2, 'secondary window background');
    win.close();
    await frames(3);
    console.log('windowed compositing: checked');
}

// A hang must not outlive the runner's timeout silently.
const watchdog = setTimeout(() => {
    assert(false, 'windowed compositing test timed out');
    window.close();
}, 30000);
run().catch((e) => assert(false, 'uncaught: ' + e)).finally(() => {
    clearTimeout(watchdog);
    window.close();
});
