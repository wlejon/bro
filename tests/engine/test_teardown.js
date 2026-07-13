// Engine teardown: load every hot spot in one process, then exit.
//
// There is no explicit assertion at the end, and that is the point — the real
// assertion is the exit code. bro-headless now destroys the Engine for real
// (it used to _exit() past ~Engine(), which meant NOTHING in this suite ever
// ran teardown, and bugs there accumulated unseen for months). A fault, a
// deadlock, or a QuickJS gc_obj_list leak assertion in the destructor chain
// all surface as a non-zero exit, which run_tests.sh reports as a failure.
//
// Every other test in the suite exercises teardown incidentally now. This one
// does it deliberately, holding the things that are hardest to tear down in the
// right order at the same time:
//
//   - canvas scenes, both attached and detached (Element->CanvasScene
//     back-pointers must be severed before the scenes are freed)
//   - a WebGL context (GL resources on the main context)
//   - a screenshot (the only thing that populates the screenshot GPU surface
//     pool — headless-only state that ~Engine() releases)
//   - a Worker (joined during binding cleanup)
//   - live timers and event listeners (JSValues that must be freed before
//     JS_FreeRuntime)

const os = require('os');
const path = require('path');

// --- canvas: attached, plus some churned into the detached list ---
const kept = document.createElement('canvas');
kept.setAttribute('width', '64');
kept.setAttribute('height', '64');
document.body.appendChild(kept);
const kctx = kept.getContext('2d');
assert(kctx, 'kept canvas has a 2d context');
kctx.fillStyle = '#3366ff';
kctx.fillRect(0, 0, 64, 64);

const churn = document.createElement('div');
document.body.appendChild(churn);
for (let i = 0; i < 8; i++) {
    const c = document.createElement('canvas');
    c.setAttribute('width', '32');
    c.setAttribute('height', '32');
    churn.appendChild(c);
    c.getContext('2d').fillRect(0, 0, 32, 32);
}
flush();
churn.textContent = '';   // scenes now orphaned; their Elements are freed lazily
flush();

// --- WebGL ---
const glCanvas = document.createElement('canvas');
glCanvas.setAttribute('width', '64');
glCanvas.setAttribute('height', '64');
document.body.appendChild(glCanvas);
const gl = glCanvas.getContext('webgl2');
assert(gl, 'webgl2 context');
gl.clearColor(0, 0, 0, 1);
gl.clear(gl.COLOR_BUFFER_BIT);
// A live GL buffer, so teardown has something to reclaim.
const buf = gl.createBuffer();
gl.bindBuffer(gl.ARRAY_BUFFER, buf);
gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([0, 0, 1, 1]), gl.STATIC_DRAW);

// --- screenshot: the only path that fills the screenshot GPU surface pool ---
const shot = path.join(os.tmpdir(), 'bro_test_teardown_' + Date.now() + '.png');
screenshot(shot);

// --- a Worker, left running (never terminated — teardown must join it) ---
const worker = new Worker('../workers/worker_basic.js');
let got = null;
worker.onmessage = (e) => { got = e.data; };
worker.postMessage({ cmd: 'echo', payload: 'ping' });
let waited = 0;
while (got === null && waited < 5000) { advanceTime(16); waited += 16; }
assert(got && got.echo === 'ping', 'worker replied');

// --- listeners + timers still live at exit ---
document.body.addEventListener('click', () => {});
kept.addEventListener('mousedown', () => {});
setInterval(() => {}, 1000);          // deliberately never cleared
setTimeout(() => {}, 999999);         // deliberately never fires

flush();

// Fall off the end with all of the above still alive. ~Engine() has to unwind
// it in the right order; if it can't, this test fails with a non-zero exit.
console.log('teardown fixture built — exiting through ~Engine()');
