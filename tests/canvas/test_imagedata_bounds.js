// ImageData sizes, readback rectangles and put sources a script chooses —
// src/bronze_host/host_canvas2d.cpp, host_imagebitmap.cpp and
// src/canvas/canvas_scene.cpp.
//
// Every number here used to reach a plain C++ cast: a NaN or a far edge was
// undefined behaviour, a negative width sized a vector of ~2^64 bytes (a
// process abort, not an exception), a huge ImageData wrapped its byte count to
// 32 bits and came back with less data than it claimed, and putImageData read
// width*height*4 bytes from a data array of any length. Each case below is now
// either the spec's answer or a thrown error — and never a crash.

function throwsNamed(fn, name) {
    try { fn(); } catch (e) { return e && e.name === name; }
    return false;
}

const c = document.createElement('canvas');
c.width = 8; c.height = 8;
document.body.appendChild(c);
const g = c.getContext('2d');
g.fillStyle = '#ff0000';
g.fillRect(0, 0, 8, 8);

// ---- createImageData ---------------------------------------------------------
assert(throwsNamed(() => g.createImageData(0, 4), 'IndexSizeError'),
       'createImageData with a zero width is an IndexSizeError');
assert(throwsNamed(() => g.createImageData(4, NaN), 'IndexSizeError'),
       'createImageData with a NaN height (0) is an IndexSizeError');
const neg = g.createImageData(-3, 2);
assert(neg.width === 3 && neg.height === 2 && neg.data.length === 24,
       'a negative side counts by its magnitude: ' + neg.width + 'x' + neg.height);
assert(throwsNamed(() => g.createImageData(1e9, 1e9), 'RangeError'),
       'createImageData past one buffer is a RangeError');
assert(throwsNamed(() => g.createImageData(Infinity, 1), 'RangeError'),
       'createImageData(Infinity, 1) is a RangeError');

// ---- new ImageData ------------------------------------------------------------
assert(throwsNamed(() => new ImageData(70000, 70000), 'RangeError'),
       'new ImageData(70000, 70000) is a RangeError, not a wrapped short buffer');
const ok = new ImageData(16, 16);
assert(ok.data.length === 16 * 16 * 4, 'a normal ImageData is unchanged');

// ---- getImageData -------------------------------------------------------------
assert(throwsNamed(() => g.getImageData(0, 0, 0, 1), 'IndexSizeError'),
       'getImageData with a zero width is an IndexSizeError');
const flipped = g.getImageData(4, 4, -2, -2);
assert(flipped.width === 2 && flipped.height === 2, 'a negative rect is the same rect from its other edge');
assert(flipped.data[0] === 255 && flipped.data[3] === 255, 'and reads the canvas at (2, 2)');
const far = g.getImageData(2147483000, -2147483000, 2, 2);
assert(far.width === 2 && far.data[3] === 0, 'a rect far off the canvas reads transparent black');
const huge = g.getImageData(1e300, 1e300, 1, 1);
assert(huge.width === 1 && huge.data[3] === 0, 'a 1e300 origin saturates instead of wrapping');
assert(throwsNamed(() => g.getImageData(0, 0, 1e6, 1e6), 'RangeError'),
       'a readback larger than one ImageData is a RangeError');

// ---- putImageData -------------------------------------------------------------
assert(throwsNamed(() => g.putImageData(
           { width: 1000, height: 1000, data: new Uint8ClampedArray(4) }, 0, 0), 'TypeError'),
       'putImageData refuses a data array shorter than width*height*4');
assert(throwsNamed(() => g.putImageData(
           { width: 1e10, height: 1e10, data: new Uint8ClampedArray(16) }, 0, 0), 'TypeError'),
       'putImageData refuses dimensions no data array could cover');

const blue = g.createImageData(2, 2);
for (let i = 0; i < blue.data.length; i += 4) { blue.data[i + 2] = 255; blue.data[i + 3] = 255; }
// Dirty rectangles past INT_MAX on every edge: clipped, never overflowed.
g.putImageData(blue, 0, 0, 1e300, -1e300, 1e300, 1e300);
g.putImageData(blue, 0, 0, -2147483648, -2147483648, 2147483647, 2147483647);
g.putImageData(blue, 1e300, -1e300);
g.putImageData(blue, 6, 6);
flush();
const put = g.getImageData(6, 6, 1, 1);
assert(put.data[2] === 255 && put.data[0] === 0, 'a normal put still lands after the wild ones');

// ---- setLineDash --------------------------------------------------------------
g.setLineDash([4, 2]);
g.setLineDash([1, NaN]);
assert(JSON.stringify(g.getLineDash()) === '[4,2]',
       'a non-finite entry ignores the call and keeps the previous dash: ' + JSON.stringify(g.getLineDash()));
g.setLineDash([3, -1]);
assert(JSON.stringify(g.getLineDash()) === '[4,2]', 'a negative entry is ignored the same way');
assert(throwsNamed(() => g.setLineDash({ length: -1 }), 'RangeError'),
       'a list with a negative length is a RangeError, not a 2^64 reserve');
assert(throwsNamed(() => g.setLineDash({ length: 1e12 }), 'RangeError'),
       'a list with an absurd length is a RangeError');

// ---- createImageBitmap crop ---------------------------------------------------
let settled = 0;
createImageBitmap(c, 2147483000, 2147483000, 2147483647, 2147483647)
    .then(() => { settled++; }, () => { settled++; });
createImageBitmap(c, -2147483648, -2147483648, 2147483647, 2147483647)
    .then((b) => { settled++; assert(b.width > 0, 'a crop clipped from INT_MIN still has pixels'); },
          () => { settled++; });
advanceTime(16);
flush();
assert(settled === 2, 'both extreme crops settled, got ' + settled);

console.log('imagedata bounds: all checks passed');
