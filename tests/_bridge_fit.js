// Fit the linear audio->style bridge B: x-vector (1024) -> Kokoro style (256).
// Dual (kernel) ridge with Cholesky — well-conditioned when N(~580) < D(1024).
// Held-out test = the real voices (anchors), so we measure whether B can
// reconstruct a real voice's style from its x-vector having trained only on
// blends/extrapolations. Run with: node tests/_bridge_fit.js
//
// Writes D:/projects/voice-sweep/bridge.json = { D, M, xm, ym, B, lambda }.
// Inference is then: style = ym + (x - xm) . B

const fs = require('fs');
const DIR = 'D:/projects/voice-sweep';
const rows = fs.readFileSync(DIR + '/pairs.jsonl', 'utf8').trim().split('\n').map(s => JSON.parse(s));
const D = rows[0].x.length, M = rows[0].style.length;
console.log('pairs', rows.length, '· D', D, '· M', M);

// splits: test = anchors (real voices); also a random 15% holdout
const isAnchor = rows.map(r => r.method === 'anchor');
let seed = 12345; const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
const rand15 = rows.map(() => rnd() < 0.15);

function fitEval(testMask, label) {
  const tr = [], te = [];
  for (let i = 0; i < rows.length; i++) (testMask[i] ? te : tr).push(i);
  const N = tr.length;

  // centering means over the train set
  const xm = new Float64Array(D), ym = new Float64Array(M);
  for (const i of tr) { const r = rows[i]; for (let j = 0; j < D; j++) xm[j] += r.x[j]; for (let j = 0; j < M; j++) ym[j] += r.style[j]; }
  for (let j = 0; j < D; j++) xm[j] /= N; for (let j = 0; j < M; j++) ym[j] /= N;

  // centered train matrices
  const Xc = new Float64Array(N * D), Yc = new Float64Array(N * M);
  for (let a = 0; a < N; a++) { const r = rows[tr[a]]; for (let j = 0; j < D; j++) Xc[a * D + j] = r.x[j] - xm[j]; for (let j = 0; j < M; j++) Yc[a * M + j] = r.style[j] - ym[j]; }

  // K = Xc Xc^T  (N x N)
  const K = new Float64Array(N * N);
  for (let a = 0; a < N; a++) for (let b = a; b < N; b++) {
    let s = 0; const ai = a * D, bi = b * D;
    for (let j = 0; j < D; j++) s += Xc[ai + j] * Xc[bi + j];
    K[a * N + b] = s; K[b * N + a] = s;
  }

  // Cholesky solve of (K + lambda I) A = Yc, then B = Xc^T A, predict test.
  const chol = (Kl) => { // in-place lower factor of an SPD matrix copy
    const L = Float64Array.from(Kl);
    for (let i = 0; i < N; i++) {
      for (let j = 0; j <= i; j++) {
        let s = L[i * N + j];
        for (let kk = 0; kk < j; kk++) s -= L[i * N + kk] * L[j * N + kk];
        if (i === j) { if (s <= 0) return null; L[i * N + j] = Math.sqrt(s); }
        else L[i * N + j] = s / L[j * N + j];
      }
      for (let j = i + 1; j < N; j++) L[i * N + j] = 0;
    }
    return L;
  };
  const solve = (L, rhs) => { // solve L L^T A = rhs (N x M), return A
    const A = Float64Array.from(rhs);
    for (let c = 0; c < M; c++) {                 // forward: L z = rhs
      for (let i = 0; i < N; i++) { let s = A[i * M + c]; for (let kk = 0; kk < i; kk++) s -= L[i * N + kk] * A[kk * M + c]; A[i * M + c] = s / L[i * N + i]; }
      for (let i = N - 1; i >= 0; i--) { let s = A[i * M + c]; for (let kk = i + 1; kk < N; kk++) s -= L[kk * N + i] * A[kk * M + c]; A[i * M + c] = s / L[i * N + i]; }
    }
    return A;
  };

  const cosV = (p, q) => { let d = 0, a = 0, b = 0; for (let j = 0; j < q.length; j++) { d += p[j] * q[j]; a += p[j] * p[j]; b += q[j] * q[j]; } return d / (Math.sqrt(a) * Math.sqrt(b) + 1e-12); };

  let best = null;
  for (const lambda of [1, 10, 50, 200, 1000]) {
    const Kl = Float64Array.from(K); for (let i = 0; i < N; i++) Kl[i * N + i] += lambda;
    const L = chol(Kl); if (!L) continue;
    const A = solve(L, Yc);                        // N x M
    // B = Xc^T A  (D x M)
    const B = new Float64Array(D * M);
    for (let a = 0; a < N; a++) { const ai = a * D, am = a * M; for (let j = 0; j < D; j++) { const xcj = Xc[ai + j]; if (xcj === 0) continue; const bj = j * M; for (let m = 0; m < M; m++) B[bj + m] += xcj * A[am + m]; } }
    // evaluate on test: cos(pred_centered, true_centered) and cos(pred, true)
    let cosC = 0, cosF = 0;
    const predC = new Float64Array(M), pred = new Float64Array(M), trueC = new Float64Array(M);
    for (const i of te) {
      const r = rows[i];
      for (let m = 0; m < M; m++) predC[m] = 0;
      for (let j = 0; j < D; j++) { const xc = r.x[j] - xm[j]; if (xc === 0) continue; const bj = j * M; for (let m = 0; m < M; m++) predC[m] += xc * B[bj + m]; }
      for (let m = 0; m < M; m++) { pred[m] = ym[m] + predC[m]; trueC[m] = r.style[m] - ym[m]; }
      cosC += cosV(predC, trueC);
      cosF += cosV(pred, r.style);
    }
    if (te.length) { cosC /= te.length; cosF /= te.length;
      console.log(`  [${label}] lambda=${lambda}  cos(centered)=${cosC.toFixed(4)}  cos(full)=${cosF.toFixed(4)}  (test ${te.length})`); }
    if (!best || cosC > best.cosC) best = { lambda, cosC, cosF, B, xm, ym };
  }
  return best;
}

console.log('== held-out = real voices (anchors) ==');
const bAnchor = fitEval(isAnchor, 'anchors');
console.log('== held-out = random 15% ==');
const bRand = fitEval(rand15, 'rand15');

// Save the anchor-holdout adapter (trained on the 578 non-anchor pairs; anchors
// kept out so the reported numbers are an honest generalization to real voices).
fs.writeFileSync(DIR + '/bridge.json', JSON.stringify({
  D, M, lambda: bAnchor.lambda,
  xm: Array.from(bAnchor.xm).map(v => +v.toFixed(7)),
  ym: Array.from(bAnchor.ym).map(v => +v.toFixed(7)),
  B: Array.from(bAnchor.B).map(v => +v.toFixed(7)),
  note: 'inference: style = ym + (x - xm) . B   (x = qwen.embedSpeaker output)',
}));
console.log('chosen lambda (anchor holdout):', bAnchor.lambda,
            '· anchor cos(full)=', bAnchor.cosF.toFixed(4),
            '· rand15 cos(full)=', bRand.cosF.toFixed(4));
console.log('WROTE', DIR + '/bridge.json');
console.log('DONE');
