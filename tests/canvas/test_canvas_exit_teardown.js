// Regression: a canvas left attached in the DOM at process exit must tear down
// cleanly. The backing Element holds a back-link to its CanvasScene; engine
// teardown frees the scenes before the Document, so it must sever those
// back-links first — otherwise ~Document -> ~Element -> onBackingElementDestroyed
// dereferences a freed CanvasScene on exit.
//
// This script just builds and draws a canvas and leaves it in the tree. The
// real assertion is implicit: the process must exit 0 without faulting in the
// destructor chain. (The test harness fails the case on a non-zero exit.)

var c = document.createElement('canvas');
c.setAttribute('width', '128');
c.setAttribute('height', '128');
document.body.appendChild(c);

var ctx = c.getContext('2d');
assert(ctx !== null && ctx !== undefined, 'getContext 2d');
ctx.fillStyle = '#2244cc';
ctx.fillRect(0, 0, 128, 128);
ctx.fillStyle = '#ffffff';
ctx.fillText('exit', 8, 24);
flush();

// A second, offscreen canvas (never appended) — exercises the GC-finalizer
// teardown path alongside the attached-element path.
var off = document.createElement('canvas');
off.setAttribute('width', '32');
off.setAttribute('height', '32');
var octx = off.getContext('2d');
octx.fillRect(0, 0, 32, 32);
flush();

assert(true, 'canvas attached at exit — teardown must not use-after-free');
