// A detached <img> held only by the listeners it is about to call stays alive
// until its load event has been delivered, as a browser keeps it.
//
// three.js's ImageLoader makes an <img>, adds load / error listeners that
// close over it, sets src and drops it: nothing in JavaScript reaches the
// element but those listeners, and nothing reaches them but the element. The
// detached-tree sweep (src/bronze_host/host_node_sweep.h) demoted that tree,
// a collection while the decode was in flight found the group dead, and the
// load then called a listener whose function and receiver were gone:
// "[bronze:IMG load listener] uncaught TypeError: undefined is not a
// function", and the texture never arrived (t_minecraft, windowed, about one
// run in three). An element with a load in flight or its load / error queued
// is now held natively.
//
// The image is a large noisy PNG so its decode spans the collections below.

const N = 2600;
const canvas = document.createElement('canvas');
canvas.width = canvas.height = N;
const g = canvas.getContext('2d');
const data = g.createImageData(N, N);
let seed = 12345;
for (let i = 0; i < data.data.length; i++) {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    data.data[i] = i % 4 === 3 ? 255 : seed & 255;
}
g.putImageData(data, 0, 0);
let url = null;
canvas.toBlob((blob) => { url = URL.createObjectURL(blob); }, 'image/png');
for (let i = 0; i < 50 && !url; i++) advanceTime(16);
assert(url, 'the PNG was encoded');

let result = null;
(function load() {
    const img = document.createElement('img');
    function onLoad() {
        img.removeEventListener('load', onLoad);
        result = this === img && img.naturalWidth === N ? 'ok' : 'wrong receiver or size';
    }
    img.addEventListener('load', onLoad);
    img.addEventListener('error', () => { result = 'error'; });
    img.src = url;
})();

// Idle seconds: each collects for the sweep (test_detached_reclaimed), the
// first two before the decode can have finished; then real time for the
// decode between them.
advanceTime(1100);
advanceTime(1100);
for (let i = 0; i < 100 && result === null; i++) {
    sleep(50);
    advanceTime(1100);
}
assert(result === 'ok', 'the held <img> delivered its load to its listener, got ' + result);
