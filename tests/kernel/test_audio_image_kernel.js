// Headless test: live bro.audio (verified brokernel DSP) and bro.image (brokernel 2D filter) pipelines.
assert(typeof bro.audio === 'object', 'bro.audio exists');
assert(typeof bro.image === 'object', 'bro.image exists');

// 1. Live bro.audio verified brokernel DSP pipeline
const bkAudioSrc = `
import std::dsp;

pub fn bk_clip(sample: f32, time: f32, drive: f32, gain: f32) -> f32 {
    return (sample * gain + 0.0 * time) * (1.0 + drive) / (1.0 + drive * abs(sample));
}
`;
const dk = bro.audio.compileDspKernel(bkAudioSrc, {
    language: 'brokernel',
    entry: 'bk_clip',
    maxAmp: 0.75,
});
assert(dk && dk.ok === true && dk.verified === true, `compileDspKernel brokernel failed: ${dk?.error}`);
assert(dk.zeroAllocProven === true && dk.nanSafeProven === true, 'DSP proofs must succeed');
const yNorm = dk.eval(0.25, 0.0, 1.0, 1.0);
assert(Math.abs(yNorm - 0.5 / 1.25) < 1e-4, `eval=${yNorm}`);
const yNan = dk.eval(NaN, 0.0, 1.0, 1.0);
const yInf = dk.eval(Infinity, 0.0, 1.0, 1.0);
const yBig = dk.eval(100.0, 0.0, 0.0, 10.0);
assert(Number.isFinite(yNan) && Math.abs(yNan) <= 0.75, 'NaN must clamp safely');
assert(Number.isFinite(yInf) && Math.abs(yInf) <= 0.75, 'Inf must clamp safely');
assert(Math.abs(yBig - 0.75) < 1e-5, `amplitude clamp=${yBig}`);

// 2. Live bro.image compiled brokernel 2D filter pipeline
assert(typeof bro.image.compileFilter === 'function', 'bro.image.compileFilter must be mounted');
const bkImgSrc = `
import std::image;

@kernel @grid(2)
pub fn k_luma_tint(g: grid<2>, src: read_store<vec4<f32>, 2>, dst: write_store<vec4<f32>, 2>) {
    let c: vec4<f32> = src.read();
    let y: f32 = image::luma709(vec3<f32>(c.x, c.y, c.z));
    dst.write(vec4<f32>(y, y * 0.5, 1.0 - c.z, c.w));
}
`;
const flt = bro.image.compileFilter(bkImgSrc, { entry: 'k_luma_tint' });
const f32In = new Float32Array([
    1.0, 0.0, 0.0, 1.0,
    0.0, 1.0, 0.0, 0.5,
]);
const f32Out = flt.applyRgbaF32(f32In, 2, 1);
assert(f32Out instanceof Float32Array && f32Out.length === 8, 'applyRgbaF32 length');
assert(Math.abs(f32Out[0] - 0.2126) < 1e-3 && Math.abs(f32Out[2] - 1.0) < 1e-4, `pixel0=${f32Out}`);
assert(Math.abs(f32Out[4] - 0.7152) < 1e-3 && Math.abs(f32Out[7] - 0.5) < 1e-4, `pixel1=${f32Out}`);

const u8In = new Uint8Array([
    255, 0, 0, 255,
    0, 255, 0, 255,
]);
const u8Out = flt.applyRgba8(u8In, 2, 1);
assert(u8Out instanceof Uint8Array && u8Out.length === 8, 'applyRgba8 length');
assert(Math.abs(u8Out[0] - Math.round(0.2126 * 255)) <= 2, `u8 pixel0=${u8Out}`);
