// Report helper for scripts/exec-matrix.sh: parses <out>/<mode>.log files,
// writes <out>/matrix.txt, prints divergences from the reference mode, and
// exits 1 if any test's verdict differs across execution modes.

const [outDir, ...modes] = Bronze.args;
if (!outDir || modes.length === 0) {
  console.error('usage: exec-matrix-report <outDir> <mode1> [mode2...]');
  Bronze.exit(2);
}

const lineRe = /^\s*(PASS|FAIL|SKIP)\s+(?:(test|smoke)\s+)?(\S+?)(?::|\s|$)/;
const ctestRe = /^\s*\d+\/\d+\s+Test\s+#\d+:\s+(\S+)\s+\.*\s*(\*\*\*)?(\w+)/;
const timingRe = /perf|budget|latency|timing|stall|jank/i;

const verdicts = new Map();
for (const m of modes) {
  const logPath = Bronze.path.join(outDir, `${m}.log`);
  const text = Bronze.fs.readText(logPath).replace(/\r\n/g, '\n');
  for (const l of text.split('\n')) {
    let hit = lineRe.exec(l);
    if (hit) {
      const name = (hit[2] ? hit[2] + ' ' : '') + hit[3];
      if (!verdicts.has(name)) verdicts.set(name, new Map());
      verdicts.get(name).set(m, hit[1]);
      continue;
    }
    hit = ctestRe.exec(l);
    if (hit) {
      const word = hit[3];
      const v = word === 'Passed' ? 'PASS' : word === 'Skipped' ? 'SKIP' : 'FAIL';
      if (!verdicts.has(hit[1])) verdicts.set(hit[1], new Map());
      verdicts.get(hit[1]).set(m, v);
    }
  }
}

const ref = modes[0];
const rows = [];
const diverged = [];
for (const name of Array.from(verdicts.keys()).sort()) {
  const v = verdicts.get(name);
  const cells = modes.map(m => v.get(m) || '-');
  rows.push(name + '\t' + cells.join('\t'));
  const base = v.get(ref);
  const odd = [];
  for (let i = 1; i < modes.length; i++) {
    const m = modes[i];
    const vm = v.get(m);
    if (vm && base && vm !== base && vm !== 'SKIP' && base !== 'SKIP') {
      odd.push([m, vm]);
    }
  }
  if (odd.length > 0) diverged.push([name, base, odd]);
}

const matrixPath = Bronze.path.join(outDir, 'matrix.txt');
Bronze.fs.writeText(matrixPath, 'test\t' + modes.join('\t') + '\n' + rows.join('\n') + '\n');
console.log(`${verdicts.size} tests; matrix in ${matrixPath}`);

if (diverged.length === 0) {
  console.log(`no test's verdict depends on the mode (${ref} is the reference)`);
  Bronze.exit(0);
}

console.log(`${diverged.length} tests differ from ${ref}:`);
for (const [name, base, odd] of diverged) {
  const tag = timingRe.test(name) ? '  timing?' : '';
  const desc = odd.map(([m, r]) => `${m} ${r}`).join(', ');
  console.log(`  ${name}: ${ref} ${base}, ${desc}${tag}`);
}
Bronze.exit(1);
