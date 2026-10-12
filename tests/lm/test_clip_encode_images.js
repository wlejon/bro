// ClipModel.encodeImages with BioCLIP 2 (bioclip-2/ under BRO_MODELS, found
// through bro.models): a batch of crops of different sizes gives, in order,
// what encodeImage gives each one alone (bit-identical on the CPU; on a GPU to
// FP16 rounding, which brolm's test_bioclip_parity measures), more images than
// one forward takes (16) are split, an empty array is an empty array, and a bad
// element throws naming its index. Skips without the weights or a GPU backend
// (the ViT-L/14 tower on the CPU takes minutes).

const lmOk = bro.lm && bro.lm.available !== false && typeof bro.lm.loadClip === 'function';
const dir = lmOk && bro.models ? bro.models.find('bioclip-2') : null;
const why = !lmOk ? 'bro.lm is the stub'
    : !dir ? 'bioclip-2 weights not found (BRO_MODELS)'
    : !bro.gpu || !bro.gpu.available ? 'no GPU backend'
    : null;

// A deterministic RGBA image with texture, different per seed.
function image(w, h, seed) {
    const data = new Uint8Array(w * h * 4);
    let s = (seed * 2654435761) >>> 0;
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
        s = (s * 1103515245 + 12345) >>> 0;
        const i = (y * w + x) * 4, n = (s >>> 24) & 63;
        data[i] = (x * 255 / w + n + seed * 40) & 255;
        data[i + 1] = (y * 255 / h + n * 2) & 255;
        data[i + 2] = ((x + y) * 3 + seed * 17) & 255;
        data[i + 3] = 255;
    }
    return { width: w, height: h, data };
}

function maxDiff(a, b) {
    let d = 0;
    for (let i = 0; i < a.length; i++) d = Math.max(d, Math.abs(a[i] - b[i]));
    return d;
}

function run() {
    const clip = bro.lm.loadClip({ vocabPath: dir + '/vocab.json', mergesPath: dir + '/merges.txt',
                                   weightsPath: dir + '/model.safetensors' });
    assert(typeof clip.encodeImages === 'function', 'encodeImages is a method');
    const dim = clip.projectionDim;

    // Crops as wildlife.analyze() makes them (224 squares), and other sizes.
    const imgs = [image(224, 224, 1), image(224, 224, 2), image(160, 120, 3), image(300, 224, 4)];
    const singles = imgs.map((im) => clip.encodeImage(im));
    const batch = clip.encodeImages(imgs);
    assert(Array.isArray(batch) && batch.length === imgs.length, 'one embedding an image: ' + batch.length);
    // A GPU computes in FP16; a crop's embedding in a batch may round
    // differently from alone (halo, Vulkan: ~3e-4).
    const tol = bro.gpu.backend === 'cpu' ? 0 : 2e-3;
    let worst = 0;
    for (let i = 0; i < imgs.length; i++) {
        assert(batch[i] instanceof Float32Array && batch[i].length === dim, 'embedding ' + i + ' is ' + dim + ' floats');
        const d = maxDiff(batch[i], singles[i]);
        worst = Math.max(worst, d);
        assert(d <= tol, 'embedding ' + i + ' is encodeImage of image ' + i + ': max |diff| ' + d);
        let n = 0;
        for (const v of batch[i]) n += v * v;
        assert(Math.abs(Math.sqrt(n) - 1) < 1e-3, 'embedding ' + i + ' is L2-normalised');
    }
    console.log('batch of ' + imgs.length + ' vs encodeImage: max |diff| ' + worst + ' (' + bro.gpu.backend + ')');
    // In order, and the images differ: no embedding is another's.
    assert(maxDiff(batch[0], singles[1]) > 10 * Math.max(worst, 1e-6), 'embeddings come back in order');

    // More than one forward pass takes (16): split, still in order.
    const many = [];
    for (let k = 0; k < 18; k++) many.push(imgs[k % imgs.length]);
    const out = clip.encodeImages(many);
    assert(out.length === 18, '18 images, 18 embeddings');
    for (let k = 0; k < 18; k++) {
        const d = maxDiff(out[k], singles[k % imgs.length]);
        assert(d <= tol, 'image ' + k + ' of 18: max |diff| ' + d);
    }

    const none = clip.encodeImages([]);
    assert(Array.isArray(none) && none.length === 0, 'no images, no embeddings');

    let err = null;
    try { clip.encodeImages([imgs[0], { width: 4, height: 4 }]); } catch (e) { err = e; }
    assert(err && /image 1/.test(String(err.message)), 'a bad image throws naming its index: ' + (err && err.message));
    err = null;
    try { clip.encodeImages(imgs[0]); } catch (e) { err = e; }
    assert(err instanceof TypeError, 'a single image (not an array) is a TypeError: ' + err);
}

if (why) skipTest(why);
else run();
