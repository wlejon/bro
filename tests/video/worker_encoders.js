// Worker for test_encoders_worker.js: encode the GIF and the WebM it is asked
// for from typed arrays, on this thread, and answer what the encoders said.
self.onmessage = (e) => {
    const { gifPath, webmPath, w, h, frames, delays } = e.data;
    const out = { gifType: typeof GifEncoder, webmType: typeof VideoEncoder };
    try {
        const px = new Uint8Array(w * h * 4);
        const fill = (i) => {
            for (let p = 0; p < w * h; p++) {
                px[p * 4] = (i * 40) & 255; px[p * 4 + 1] = (p % w) * 4; px[p * 4 + 2] = 128; px[p * 4 + 3] = 255;
            }
        };
        const gif = new GifEncoder({ path: gifPath, width: w, height: h });
        out.gifInstance = gif instanceof GifEncoder;
        for (let i = 0; i < frames; i++) {
            fill(i);
            gif.setNextFrameDelayCs(delays[i]);
            gif.addFrameRGBA(px);
        }
        out.gifFinished = gif.finish();

        const webm = new VideoEncoder({ path: webmPath, width: w, height: h, fps: 10 });
        for (let i = 0; i < frames; i++) { fill(i); webm.addFrameRGBA(px); }
        out.webmFinished = webm.finish();
        out.webmFrames = webm.framesWritten;

        // The DOM-bound sources are refused with a message naming the one that works.
        try { webm.addViewportFrame(); out.viewport = 'accepted'; } catch (err) { out.viewport = `${err.name}: ${err.message}`; }
        try { gif.addCanvasFrame({}); out.canvas = 'accepted'; } catch (err) { out.canvas = `${err.name}: ${err.message}`; }
    } catch (err) {
        out.error = String((err && err.stack) || err);
    }
    self.postMessage(out);
};
