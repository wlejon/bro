// bro pumps brodiffusion's async jobs from the engine frame pump
// (host_sibling_apis.cpp: tickDiffusionAsync each frame, shutdownDiffusionAsync
// at teardown). A background generate's onDone is delivered by the frames
// alone. Nothing here calls bro.diffusion.tick(), pipe.tick() or handle.wait().
//
// Needs the SD1.5 diffusers export. Set BRO_DIFFUSION_SD15 to override the
// default path ../brodiffusion/weights/sd15, which is relative to the working
// directory. The test skips when the weights or a GPU are missing, and uses
// 2 steps at 256x256, so the run is short.

const fs = require('node:fs');
const DIR = process.env.BRO_DIFFUSION_SD15 || (process.cwd() + '/../brodiffusion/weights/sd15');

if (bro.diffusion.available === false) {
    console.log('SKIP: bro.diffusion is the unavailable stub');
} else if (!fs.existsSync(DIR + '/model_index.json')) {
    console.log('SKIP: SD1.5 weights not found at ' + DIR);
} else if (!bro.gpu.available) {
    console.log('SKIP: no GPU backend (' + bro.gpu.backend + ')');
} else {
    // Frames only: advanceTime runs the engine's frame pumps, and wallSleep
    // gives the generate's worker thread real time to run.
    function pump(pred, timeoutMs, what) {
        const t0 = Date.now();
        while (Date.now() - t0 < timeoutMs) {
            advanceTime(16);
            if (pred()) return true;
            wallSleep(5);
        }
        assert(false, what + ' did not complete within ' + timeoutMs + 'ms');
        return false;
    }

    const pipe = bro.diffusion.loadModel(DIR);
    assert(pipe && !pipe.cancelled, 'loadModel returned a pipeline');

    let result = null, info = null, calls = 0;
    const handle = pipe.generateAsync('a red cube on a table', {
        width: 256, height: 256, steps: 2, seed: 1,
        onDone(r, i) { calls++; result = r; info = i; },
    });
    assert(handle && typeof handle.cancel === 'function', 'generateAsync returned a job handle');
    assert(calls === 0, 'onDone does not run synchronously');

    pump(() => calls > 0, 180000, 'background generate');
    assert(calls === 1, 'onDone fired exactly once, got ' + calls);
    assert(info && !info.cancelled && !info.error, 'run finished cleanly: ' + JSON.stringify(info));
    assert(result && result.width === 256 && result.height === 256,
        'ImageResult is 256x256, got ' + (result && result.width) + 'x' + (result && result.height));
    assert(result.data instanceof Uint8ClampedArray && result.data.length === 256 * 256 * 4,
        'RGBA pixels delivered');
    assert(handle.done === true, 'handle reports done');
    assert(pipe.busy === false, 'pipeline is free again after onDone');

    // A second job left in flight is the shutdown hook's to cancel and join
    // at teardown. If it isn't joined, the run hangs or crashes on exit.
    pipe.generateAsync('a blue sphere', { width: 256, height: 256, steps: 2, seed: 2, onDone() {} });

    console.log('test_diffusion_async_tick: OK');
}
