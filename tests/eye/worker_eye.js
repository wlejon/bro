// Worker helper for tests/eye/test_eye_measure_compare.js
onmessage = (e) => {
    const { width, height, pixels } = e.data;
    const frame = { width, height, data: new Uint8ClampedArray(pixels) };
    const m = bro.eye.measure(frame);
    const c = bro.eye.compare(frame, frame);
    postMessage({
        available: bro.eye.available,
        hasAudit: typeof bro.eye.audit,
        lumaMean: m.lumaMean,
        selfScore: c.score,
        selfSsim: c.ssim,
    });
};
