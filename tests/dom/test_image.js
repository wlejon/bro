// Test HTMLImageElement / Image constructor — exercises
// src/js/image_bindings.cpp (the DOM Image wrapper, not bro.image).

// Constructor exists and creates an object
const img = new Image();
assert(img !== null, 'new Image() works');
assert(img instanceof Image, 'instance is Image');
assert(img instanceof HTMLImageElement, 'instance is HTMLImageElement');

// Initial state
assert(img.width === 0, 'initial width = 0');
assert(img.height === 0, 'initial height = 0');
assert(img.naturalWidth === 0, 'initial naturalWidth = 0');
assert(img.naturalHeight === 0, 'initial naturalHeight = 0');
assert(img.complete === false, 'initial complete = false');
assert(img.src === '', 'initial src empty');

// =========================================================================
// onload callback fires on src assignment (sync — stb_image is synchronous)
// =========================================================================
let loadFired = false;
img.onload = function() {
    loadFired = true;
};

// Set src to a non-existent path. The binding falls back to a 1x1 white
// pixel rather than failing, and still fires onload synchronously.
img.src = '/nonexistent.png';
assert(img.complete === true, 'complete set after src assignment');
assert(img.width >= 1, 'width set after load');
assert(img.height >= 1, 'height set after load');
assert(loadFired === true, 'onload fired');

// naturalWidth/Height mirror width/height
assert(img.naturalWidth === img.width, 'naturalWidth = width');
assert(img.naturalHeight === img.height, 'naturalHeight = height');

// =========================================================================
// addEventListener('load') alternative
// =========================================================================
const img2 = new Image();
let loadEvFired = false;
img2.addEventListener('load', () => { loadEvFired = true; });
img2.src = '/also-nonexistent.png';
assert(loadEvFired === true, 'load event fired via addEventListener');

// removeEventListener clears
const img3 = new Image();
const handler = () => { throw new Error('should not fire'); };
img3.addEventListener('load', handler);
img3.removeEventListener('load', handler);
img3.src = '/foo.png';
// (no throw = success)

// =========================================================================
// src getter echoes the assignment
// =========================================================================
const img4 = new Image();
img4.src = '/some/path.png';
assert(img4.src.indexOf('some/path.png') !== -1, 'src getter echoes, got: ' + img4.src);

// =========================================================================
// onload assignment replaces previous callback
// =========================================================================
const img5 = new Image();
let count = 0;
img5.onload = () => count++;
img5.onload = () => count += 10; // replaces
img5.src = '/x.png';
assert(count === 10, 'onload assignment replaces, got count=' + count);
