// GC pause probe: holds N live objects, then times 200 batches that each
// allocate 20000 short-lived objects. A batch's time includes any collection
// it triggered, so `worst` is the longest pause-plus-work a frame would feel
// and `avg` the steady cost. Usage: bro-headless <app> gc_pause.js -- <N>
const N = Number((typeof scriptArgs !== 'undefined' && scriptArgs[0]) || 500000);
const live = [];
for (let i = 0; i < N; i++) live.push({ a: i, b: [i, i + 1], c: 's' + (i % 100) });
let worst = 0, total = 0;
for (let b = 0; b < 200; b++) {
  const s = Date.now();
  let t = [];
  for (let j = 0; j < 20000; j++) t.push({ x: j, y: [j] });
  const d = Date.now() - s;
  total += d;
  if (d > worst) worst = d;
}
console.log('GCPROBE live=' + live.length + ' worst=' + worst + ' avg=' + (total / 200).toFixed(2));
