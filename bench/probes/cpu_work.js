// CPU-heavy driver for bro-headless: object math in functions, a hot
// top-level loop, and array/string work. Times with Date.now (the headless
// performance clock is virtual).
function Body(x, y, z, vx, vy, vz, m) {
  this.x = x; this.y = y; this.z = z;
  this.vx = vx; this.vy = vy; this.vz = vz; this.m = m;
}
function advance(bodies, dt) {
  const n = bodies.length;
  for (let i = 0; i < n; i++) {
    const a = bodies[i];
    for (let j = i + 1; j < n; j++) {
      const b = bodies[j];
      const dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
      const d2 = dx * dx + dy * dy + dz * dz + 0.01;
      const mag = dt / (d2 * Math.sqrt(d2));
      a.vx -= dx * b.m * mag; a.vy -= dy * b.m * mag; a.vz -= dz * b.m * mag;
      b.vx += dx * a.m * mag; b.vy += dy * a.m * mag; b.vz += dz * a.m * mag;
    }
  }
  for (let i = 0; i < n; i++) {
    const a = bodies[i];
    a.x += dt * a.vx; a.y += dt * a.vy; a.z += dt * a.vz;
  }
}
function energy(bodies) {
  let e = 0;
  for (const b of bodies) e += 0.5 * b.m * (b.vx * b.vx + b.vy * b.vy + b.vz * b.vz);
  return e;
}

const t0 = Date.now();
const bodies = [];
for (let i = 0; i < 40; i++) {
  bodies.push(new Body(Math.sin(i), Math.cos(i * 1.3), i * 0.01, 0, 0, 0, 1 + (i % 3)));
}
for (let s = 0; s < 3000; s++) advance(bodies, 0.001);
const t1 = Date.now();

// A hot loop at top level: runs once, so only OSR can compile it.
let acc = 0;
const arr = new Float64Array(4096);
for (let k = 0; k < 4096; k++) arr[k] = (k * 7919) % 1000;
for (let r = 0; r < 3000; r++) {
  for (let k = 0; k < 4096; k++) {
    acc += arr[k] * ((r & 7) + 1);
    arr[k] = (arr[k] + r) % 1000;
  }
}
const t2 = Date.now();

let words = [];
for (let i = 0; i < 20000; i++) words.push('w' + (i * 31 % 977));
const counts = new Map();
for (let rep = 0; rep < 20; rep++) {
  for (const w of words) counts.set(w, (counts.get(w) || 0) + 1);
}
const t3 = Date.now();

console.log('CPU nbody=' + (t1 - t0) + 'ms toplevel=' + (t2 - t1) + 'ms strings=' + (t3 - t2) +
            'ms total=' + (t3 - t0) + 'ms check=' + energy(bodies).toFixed(6) + ' ' + acc + ' ' + counts.size);
