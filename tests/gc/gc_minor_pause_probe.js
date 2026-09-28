// Minor-collection pauses against the size of the old generation.
//
//   bro-headless tests/_smoke_app tests/gc/gc_minor_pause_probe.js -- <live> [batches]
//
// Builds <live> long-lived objects (they are promoted by the first minor
// collections), then runs batches that each allocate 20k short-lived objects
// and store a few young objects into old ones, so the remembered set is not
// empty. A minor collection should cost what the young generation and the
// dirty cards hold, not what the old generation holds: the batch times (and,
// with BRASS_GC_LOG=1, each collection's pause split into roots, cards and
// trace) should stay flat as <live> grows.

const LIVE = Number((typeof scriptArgs !== 'undefined' && scriptArgs[0]) || 20000);
const BATCHES = Number((typeof scriptArgs !== 'undefined' && scriptArgs[1]) || 300);
const PER_BATCH = 20000;

const live = new Array(LIVE);
for (let i = 0; i < LIVE; i++) live[i] = { id: i, name: 'n' + (i & 1023), next: null, tag: i & 7 };
for (let i = 1; i < LIVE; i++) live[i].next = live[i - 1];

let sink = null;
const times = [];
for (let b = 0; b < BATCHES; b++) {
  const s = Date.now();
  for (let i = 0; i < PER_BATCH; i++) {
    const o = { x: i, y: b, z: sink };
    // Old-to-young stores: into a live object's field, and a live slot of the
    // (large, old) array replaced outright.
    if ((i & 1023) === 0) live[(b * 7919 + i) % LIVE].next = o;
    if ((i & 1023) === 512) live[(b * 104729 + i) % LIVE] = { id: i, name: 'r', next: null, tag: 0 };
    sink = (i & 7) === 0 ? null : o;
  }
  times.push(Date.now() - s);
}

times.sort((a, b) => a - b);
const sum = times.reduce((a, b) => a + b, 0);
console.log(`[gcprobe] live ${LIVE} batches ${BATCHES} mean ${(sum / BATCHES).toFixed(2)} ms ` +
            `p50 ${times[BATCHES >> 1]} ms p99 ${times[Math.floor(BATCHES * 0.99)]} ms max ${times[BATCHES - 1]} ms`);
