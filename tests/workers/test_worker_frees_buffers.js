// A Worker that makes big buffers and drops them frees them. Two things kept
// them: the worker's turn ran the checkpoint (microtasks, and with them the
// deferred finalizers that free a buffer's external block) only when a
// microtask was queued, which a synchronous onmessage never queues; and the
// external bytes brought no collection round, so a thread allocating little
// else on its heap never collected. The Image Viewer's decode workers, 24 MB a
// photo, held every photo of a card until the machine ran out of memory.
const MB = 1 << 20;
// The process's private commit (__host.memory: Windows only; elsewhere the
// worker still runs, unmeasured).
const committed = () => {
  const m = __host.memory();
  return m && m.heapCommitted !== undefined ? m.heapCommitted + m.outsideHeaps : NaN;
};

const w = new Worker('../workers/worker_drop_buffers.js');
let replies = 0;
w.onmessage = () => { replies++; };
const send = (n) => {
  const want = replies + n;
  for (let i = 0; i < n; i++) w.postMessage({ bytes: 16 * MB });
  for (let t = 0; t < 20000 && replies < want; t++) { wallSleep(1); advanceTime(1); }
  assert(replies === want, `the worker answered ${replies} of ${want}`);
};

send(8);
const before = committed();
// 2 GB made and dropped, one buffer a message.
for (let i = 0; i < 16; i++) send(8);
const grown = (committed() - before) / MB;
console.log(`128 x 16 MB made and dropped in a worker: committed grew ${grown.toFixed(0)} MB`);
if (!Number.isNaN(grown)) assert(grown < 512,`the worker's dropped buffers were freed (committed grew ${grown.toFixed(0)} MB of 2048)`);
w.terminate();
