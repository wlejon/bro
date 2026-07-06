// ImageData is a real class: the global constructor has a prototype and
// every producer (new ImageData, ctx.getImageData, ctx.createImageData)
// returns instances of it. Regression: ImageData was a bare C-function
// constructor with NO .prototype, so `x instanceof ImageData` THREW
// ("operand 'prototype' property is not an object") — which killed any app
// using the standard CanvasImageSource type switch (krea2-lab's
// paintMintThumb died on every image-pair pick).

assert(typeof ImageData === 'function', 'ImageData global exists');
assert(typeof ImageData.prototype === 'object' && ImageData.prototype !== null,
       'ImageData has a prototype object');
assert(ImageData.prototype.constructor === ImageData,
       'prototype.constructor round-trips');

// instanceof must ANSWER (not throw) for arbitrary values.
assert(({}) instanceof ImageData === false, 'plain object is not an ImageData');
assert(document.createElement('canvas') instanceof ImageData === false,
       'canvas is not an ImageData');

// --- new ImageData(w, h) ----------------------------------------------------
const a = new ImageData(4, 3);
assert(a instanceof ImageData, 'new ImageData(w, h) is an instance');
assert(a.width === 4 && a.height === 3, 'dimensions stored');
assert(a.data instanceof Uint8ClampedArray, 'data is Uint8ClampedArray');
assert(a.data.length === 4 * 3 * 4, 'data sized w*h*4');
assert(a.data[0] === 0, 'allocated zeroed');

// --- new ImageData(data, w [, h]) — shares the buffer -----------------------
const px = new Uint8ClampedArray(2 * 2 * 4).fill(7);
const b = new ImageData(px, 2, 2);
assert(b instanceof ImageData, 'new ImageData(data, w, h) is an instance');
assert(b.data === px, 'data-first form shares the array');

// --- ctx.getImageData / ctx.createImageData ---------------------------------
const c = document.createElement('canvas');
c.width = 8; c.height = 8;
const g = c.getContext('2d');
g.fillStyle = '#ff0000';
g.fillRect(0, 0, 8, 8);

const got = g.getImageData(0, 0, 8, 8);
assert(got instanceof ImageData, 'getImageData returns an ImageData instance');
assert(got.width === 8 && got.height === 8, 'getImageData dimensions');
assert(got.data[0] === 255 && got.data[1] === 0, 'getImageData pixels read back');

const made = g.createImageData(5, 6);
assert(made instanceof ImageData, 'createImageData returns an ImageData instance');
assert(made.width === 5 && made.height === 6 && made.data.length === 5 * 6 * 4,
       'createImageData shape');

// putImageData still consumes the real instances (round trip).
made.data.fill(128);
g.putImageData(made, 0, 0);
const back = g.getImageData(0, 0, 1, 1);
assert(back.data[0] === 128, 'putImageData(instance) round-trips, got ' + back.data[0]);
