// The frame loop pumps media events for the <video> elements the document
// keeps (Document::videoElements) instead of walking every element to find
// them. The same ones hear their events as before: one in the page, one
// inside a shadow tree, one slotted into one; not one detached from the
// document, until it is put back; in document order; and a handler that
// removes a later video, or takes the page's video away, breaks nothing.

const os = require('os');
const path = require('path');

const file = path.join(os.tmpdir(), 'bro_video_registry_' + Date.now() + '.webm');
const W = 32, H = 24;
const enc = new VideoEncoder({ path: file, width: W, height: H, fps: 5, fpsDen: 1, quality: 'realtime' });
const px = new Uint8Array(W * H * 4).fill(120);
for (let f = 0; f < 5; f++) enc.addFrameRGBA(px);
enc.finish();
const url = file.split('\\').join('/');

function pumpUntil(pred) {
    const t = Date.now();
    while (!pred() && Date.now() - t < 10000) { sleep(10); flush(); advanceTime(20); }
    return pred();
}

const root = document.getElementById('root');
const order = [];
function video(name) {
    const v = document.createElement('video');
    v.id = name;
    v.addEventListener('loadedmetadata', () => order.push(name));
    return v;
}

// In the page, inside a shadow tree, and slotted into one.
const plain = video('plain');
root.appendChild(plain);
const host = document.createElement('div');
root.appendChild(host);
const shadow = host.attachShadow({ mode: 'open' });
const inner = video('shadowed');
shadow.appendChild(document.createElement('div'));
shadow.firstChild.appendChild(inner);
shadow.firstChild.appendChild(document.createElement('slot'));
const slotted = video('slotted');
host.appendChild(slotted);
// Detached: never in the document.
const loose = video('loose');
// A light child of a host no slot takes is not rendered either.
const host2 = document.createElement('div');
root.appendChild(host2);
host2.attachShadow({ mode: 'open' }).appendChild(document.createElement('div'));
const unslotted = video('unslotted');
host2.appendChild(unslotted);

for (const v of [plain, inner, slotted, loose, unslotted]) { v.src = url; v.load(); }
assert(pumpUntil(() => order.length >= 3), `three connected videos loaded, saw ${order}`);
for (let i = 0; i < 10; i++) { flush(); advanceTime(20); }
assert(order.join(',') === 'plain,shadowed,slotted',
       `loadedmetadata in document order for the rendered videos only, got ${order}`);

// Put back, the detached one is pumped too.
root.appendChild(loose);
assert(pumpUntil(() => order.includes('loose')), `a video put back in the document hears its events: ${order}`);

// A handler that removes the next video and frees the first: nothing breaks.
const a = video('a'), b = video('b');
root.appendChild(a);
root.appendChild(b);
a.addEventListener('loadedmetadata', () => { b.remove(); plain.remove(); });
a.src = url; a.load();
b.src = url; b.load();
assert(pumpUntil(() => order.includes('a')), `the first of the pair loaded: ${order}`);
for (let i = 0; i < 10; i++) { flush(); advanceTime(20); }
assert(!order.includes('b'), `a video removed by an earlier handler is not pumped: ${order}`);
console.log('video registry: ' + order.join(','));
