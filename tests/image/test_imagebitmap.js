// Test ImageBitmap global and basic lifecycle
// Exercises src/js/imagebitmap_bindings.cpp

assert(typeof ImageBitmap === 'function', 'ImageBitmap constructor exists');
assert(typeof createImageBitmap === 'function', 'createImageBitmap global exists');
assert(typeof ImageData === 'function', 'ImageData constructor exists');

// ImageBitmap constructor throws TypeError per spec (illegal constructor)
let threwIllegal = false;
try {
    new ImageBitmap();
} catch (e) {
    threwIllegal = true;
    assert(e instanceof TypeError, 'ImageBitmap ctor throws TypeError');
}
assert(threwIllegal, 'new ImageBitmap() throws illegal constructor error');

// ImageData constructors
const id1 = new ImageData(8, 8);
assert(id1 instanceof ImageData, 'id1 is instanceof ImageData');
assert(id1.width === 8 && id1.height === 8, 'id1 dimensions match 8x8');
assert(id1.data instanceof Uint8ClampedArray, 'id1.data is Uint8ClampedArray');
assert(id1.data.length === 8 * 8 * 4, 'id1.data length matches 8*8*4');

const raw = new Uint8ClampedArray(4 * 4 * 4);
raw.fill(128);
const id2 = new ImageData(raw, 4, 4);
assert(id2 instanceof ImageData, 'id2 is instanceof ImageData');
assert(id2.width === 4 && id2.height === 4, 'id2 dimensions match 4x4');
assert(id2.data === raw, 'id2 shares the provided Uint8ClampedArray');

// Create ImageBitmap from ImageData
const bmp = await createImageBitmap(id2);
assert(bmp instanceof ImageBitmap, 'bmp is instanceof ImageBitmap');
assert(bmp.width === 4 && bmp.height === 4, 'bmp dimensions match source');

// Create ImageBitmap from duck-typed source
const duckBmp = await createImageBitmap({
    width: 2,
    height: 2,
    data: new Uint8ClampedArray(2 * 2 * 4),
});
assert(duckBmp instanceof ImageBitmap, 'duckBmp is instanceof ImageBitmap');
assert(duckBmp.width === 2 && duckBmp.height === 2, 'duckBmp dimensions match');

// Cropped creation
const croppedBmp = await createImageBitmap(id2, 1, 1, 2, 2);
assert(croppedBmp instanceof ImageBitmap, 'croppedBmp is instanceof ImageBitmap');
assert(croppedBmp.width === 2 && croppedBmp.height === 2, 'cropped dimensions match 2x2');

// close() clears dimensions
assert(bmp.width === 4 && bmp.height === 4, 'before close dimensions valid');
bmp.close();
assert(bmp.width === 0 && bmp.height === 0, 'after close dimensions are 0');

duckBmp.close();
croppedBmp.close();

console.log('test_imagebitmap: passed');
