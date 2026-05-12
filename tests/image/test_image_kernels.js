// Test bro.image typed-array kernels — reduce, map, combine, lookup,
// stencil, resample, gradient, alloc. Exercises bro.image namespace
// in src/js/image_bindings.cpp (and the kernel impls in brokit-like
// shared code).

assert(typeof bro === 'object', 'bro namespace');
assert(typeof bro.image === 'object', 'bro.image namespace');

// =========================================================================
// alloc
// =========================================================================
const f32 = bro.image.alloc(16, 16, 1);
assert(f32 instanceof Float32Array, 'alloc default = Float32Array');
assert(f32.length === 16 * 16, 'alloc length = w*h*channels');

const rgba = bro.image.alloc(8, 8, 4, 'uint8c');
assert(rgba instanceof Uint8ClampedArray, 'uint8c -> Uint8ClampedArray');
assert(rgba.length === 8 * 8 * 4, 'rgba size');

const u8 = bro.image.alloc(4, 4, 1, 'uint8');
assert(u8 instanceof Uint8Array, 'uint8 -> Uint8Array');

const i32 = bro.image.alloc(2, 2, 1, 'int32');
assert(i32 instanceof Int32Array, 'int32 -> Int32Array');

// =========================================================================
// reduce — minmax, sum, mean, histogram
// =========================================================================
const arr = new Float32Array([1, 2, 3, 4, 5]);

const mm = bro.image.reduce(arr, 'minmax');
assert(typeof mm === 'object', 'minmax returns object');
assert(mm.min === 1, 'min = 1');
assert(mm.max === 5, 'max = 5');

const sum = bro.image.reduce(arr, 'sum');
assert(sum === 15, 'sum = 15');

const mean = bro.image.reduce(arr, 'mean');
assert(Math.abs(mean - 3) < 0.001, 'mean = 3');

const hist = bro.image.reduce(arr, 'histogram', { bins: 5, lo: 1, hi: 5 });
assert(hist instanceof Uint32Array, 'histogram returns Uint32Array');
assert(hist.length === 5, 'histogram has 5 bins');

// =========================================================================
// map — affine, abs, sqrt, exp, log, pow
// =========================================================================
const src = new Float32Array([0, 1, 2, 3, 4]);
const dst = new Float32Array(5);

bro.image.map(dst, src, { op: 'affine', a: 2, b: 1 });
assert(dst[0] === 1, 'affine 0*2+1 = 1');
assert(dst[4] === 9, 'affine 4*2+1 = 9');

bro.image.map(dst, src, { op: 'affine', a: 1, b: 0, clamp: [0, 2] });
assert(dst[0] === 0, 'clamp lower');
assert(dst[4] === 2, 'clamp upper');

const neg = new Float32Array([-1, -2, 3]);
const absDst = new Float32Array(3);
bro.image.map(absDst, neg, { op: 'abs' });
assert(absDst[0] === 1, 'abs of -1');
assert(absDst[1] === 2, 'abs of -2');
assert(absDst[2] === 3, 'abs of 3');

bro.image.map(dst, src, { op: 'sqrt' });
assert(Math.abs(dst[4] - 2) < 0.001, 'sqrt(4) = 2');

bro.image.map(dst, src, { op: 'pow', exp: 2 });
assert(dst[2] === 4, 'pow(2,2) = 4');
assert(dst[3] === 9, 'pow(3,2) = 9');

// =========================================================================
// combine — add, sub, mul, min, max, lerp, wsum
// =========================================================================
const a = new Float32Array([1, 2, 3, 4]);
const b = new Float32Array([10, 20, 30, 40]);
const out = new Float32Array(4);

bro.image.combine(out, a, b, { op: 'add' });
assert(out[0] === 11, 'add[0] = 11');
assert(out[3] === 44, 'add[3] = 44');

bro.image.combine(out, b, a, { op: 'sub' });
assert(out[0] === 9, 'sub[0] = 9');

bro.image.combine(out, a, b, { op: 'mul' });
assert(out[0] === 10, 'mul[0] = 10');
assert(out[3] === 160, 'mul[3] = 160');

bro.image.combine(out, a, b, { op: 'min' });
assert(out[0] === 1, 'min[0] = 1');

bro.image.combine(out, a, b, { op: 'max' });
assert(out[0] === 10, 'max[0] = 10');

bro.image.combine(out, a, b, { op: 'lerp', t: 0.5 });
assert(out[0] === 5.5, 'lerp 0.5 = 5.5');

bro.image.combine(out, a, b, { op: 'wsum', wa: 2, wb: 1 });
assert(out[0] === 12, 'wsum 2*1 + 1*10 = 12');

// =========================================================================
// lookup — scalar field through RGBA8 LUT
// =========================================================================
const lut = bro.image.gradient([
    [0.0, 0, 0, 0],
    [1.0, 255, 255, 255],
], 256);
assert(lut instanceof Uint8Array, 'gradient returns Uint8Array');
assert(lut.length === 256 * 4, 'lut size');
// First entry near black, last near white
assert(lut[0] < 10, 'lut start ~black');
assert(lut[(255) * 4] > 245, 'lut end ~white');

const field = new Float32Array([0.0, 0.5, 1.0, 0.25]);
const outRgba = new Uint8Array(4 * 4);
bro.image.lookup(outRgba, field, lut, { lo: 0, hi: 1 });
// First pixel near black, third near white
assert(outRgba[0] < 10, 'first pixel ~black');
assert(outRgba[8] > 245, 'third pixel (t=1.0) ~white');

// =========================================================================
// gradient with multiple stops
// =========================================================================
const ramp = bro.image.gradient([
    [0.0, 255, 0, 0],
    [0.5, 0, 255, 0],
    [1.0, 0, 0, 255],
]);
assert(ramp.length === 256 * 4, 'multi-stop gradient default 256');
// At t=0: red
assert(ramp[0] > 200, 't=0 red high');
// At t=0.5 (~index 128): green dominant
const midR = ramp[128 * 4];
const midG = ramp[128 * 4 + 1];
assert(midG > midR, 'mid: green > red');
// At t=1: blue
const endB = ramp[255 * 4 + 2];
assert(endB > 200, 't=1 blue high');

// =========================================================================
// stencil — 3x3 convolution (blur)
// =========================================================================
const W = 4, H = 4;
const stencSrc = new Float32Array(W * H);
for (let i = 0; i < W * H; ++i) stencSrc[i] = i;
const blurOut = new Float32Array(W * H);
const blur = { data: new Float32Array([1,1,1, 1,1,1, 1,1,1]), w: 3, h: 3 };
bro.image.stencil(blurOut, stencSrc, blur, {
    srcW: W, srcH: H, edge: 'clamp', divisor: 9,
});
// Output should be a smoothed version — central elements close to neighborhood mean
assert(typeof blurOut[5] === 'number', 'blur produces numbers');

// Edge mode 'wrap'
bro.image.stencil(blurOut, stencSrc, blur, {
    srcW: W, srcH: H, edge: 'wrap', divisor: 9,
});

// Edge mode 'zero'
bro.image.stencil(blurOut, stencSrc, blur, {
    srcW: W, srcH: H, edge: 'zero', divisor: 9,
});

// Sobel X
const sobel = { data: new Float32Array([-1,0,1, -2,0,2, -1,0,1]), w:3, h:3 };
bro.image.stencil(blurOut, stencSrc, sobel, {
    srcW: W, srcH: H, edge: 'clamp',
});

// =========================================================================
// resample — bilinear and nearest
// =========================================================================
const smallW = 2, smallH = 2;
const small = new Float32Array([0, 1, 2, 3]);
const bigW = 4, bigH = 4;
const big = new Float32Array(bigW * bigH);
bro.image.resample(big, small, {
    srcW: smallW, srcH: smallH, dstW: bigW, dstH: bigH,
    channels: 1, filter: 'bilinear',
});
// Output has finite values
assert(!isNaN(big[0]), 'resample bilinear produces numbers');

bro.image.resample(big, small, {
    srcW: smallW, srcH: smallH, dstW: bigW, dstH: bigH,
    channels: 1, filter: 'nearest',
});
assert(!isNaN(big[0]), 'resample nearest produces numbers');

// =========================================================================
// Cleanup — nothing to clean
// =========================================================================
