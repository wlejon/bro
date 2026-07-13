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

// A real PNG on disk to load — screenshot() gives us one without checking a
// binary asset into the tree. Absolute path, so it bypasses app-relative src
// resolution. Decoding is synchronous (stb_image), so load/error fires before
// the `src` assignment returns.
const os = require('os');
const path = require('path');
const REAL_PNG = path.join(os.tmpdir(), 'bro_test_image_' + Date.now() + '.png');
screenshot(REAL_PNG);

// =========================================================================
// onload fires on a successful decode
// =========================================================================
let loadFired = false;
let errFired = false;
img.onload = function() { loadFired = true; };
img.onerror = function() { errFired = true; };

img.src = REAL_PNG;
assert(img.complete === true, 'complete set after src assignment');
assert(loadFired === true, 'onload fired on success');
assert(errFired === false, 'onerror did NOT fire on success');
assert(img.width > 1, 'width set from the decoded image');
assert(img.height > 1, 'height set from the decoded image');

// naturalWidth/Height mirror width/height
assert(img.naturalWidth === img.width, 'naturalWidth = width');
assert(img.naturalHeight === img.height, 'naturalHeight = height');

// =========================================================================
// onerror fires on a failed decode — a missing asset is a *broken image*,
// not a silent 1x1 white one. Broken => zero natural size, no pixels.
// =========================================================================
const bad = new Image();
let badLoad = false, badErr = false;
bad.onload = () => { badLoad = true; };
bad.onerror = () => { badErr = true; };

bad.src = '/nonexistent.png';
assert(badErr === true, 'onerror fired on missing file');
assert(badLoad === false, 'onload did NOT fire on missing file');
assert(bad.complete === true, 'complete is true even on error (fetch settled)');
assert(bad.width === 0, 'broken image has zero width, got ' + bad.width);
assert(bad.height === 0, 'broken image has zero height, got ' + bad.height);
assert(bad.naturalWidth === 0, 'broken image has zero naturalWidth');

// A throwing handler is reported by the error funnel, not left pending —
// the next binding call must still work.
const thrower = new Image();
thrower.onerror = () => { throw new Error('handler blew up (expected)'); };
thrower.src = '/nonexistent-2.png';
assert(thrower.complete === true, 'src setter survives a throwing onerror');

// =========================================================================
// addEventListener('load' / 'error') alternative
// =========================================================================
const img2 = new Image();
let loadEvFired = false;
img2.addEventListener('load', () => { loadEvFired = true; });
img2.src = REAL_PNG;
assert(loadEvFired === true, 'load event fired via addEventListener');

const img2e = new Image();
let errEvFired = false;
img2e.addEventListener('error', () => { errEvFired = true; });
img2e.src = '/also-nonexistent.png';
assert(errEvFired === true, 'error event fired via addEventListener');

// removeEventListener clears
const img3 = new Image();
const handler = () => { throw new Error('should not fire'); };
img3.addEventListener('load', handler);
img3.removeEventListener('load', handler);
img3.src = REAL_PNG;

const img3e = new Image();
const eHandler = () => { throw new Error('should not fire'); };
img3e.addEventListener('error', eHandler);
img3e.removeEventListener('error', eHandler);
img3e.src = '/foo.png';
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
img5.src = REAL_PNG;
assert(count === 10, 'onload assignment replaces, got count=' + count);

// onerror assignment replaces too
const img6 = new Image();
let ecount = 0;
img6.onerror = () => ecount++;
img6.onerror = () => ecount += 10; // replaces
img6.src = '/x.png';
assert(ecount === 10, 'onerror assignment replaces, got ecount=' + ecount);

// A successful load must not leave a stale broken state behind: reusing an
// Image for a good src after a bad one has to recover its dimensions.
const reused = new Image();
reused.src = '/gone.png';
assert(reused.naturalWidth === 0, 'broken after bad src');
reused.src = REAL_PNG;
assert(reused.naturalWidth > 0, 'recovers dimensions after a good src');
assert(reused.complete === true, 'complete after recovery');
