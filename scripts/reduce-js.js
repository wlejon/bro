// reduce-js.js — shrink a JS file while a bro-headless run of it still shows a symptom.
//
//   bronze run scripts/reduce-js.js -- <file.js> <needle> [--appdir DIR] [--env K=V ...]
//                                      [--timeout S] [--absent] [-- extra bro-headless args]

const rawArgs = Bronze.args.slice();
let extra = [];
const sep = rawArgs.indexOf('--');
const argv = sep >= 0 ? rawArgs.slice(0, sep) : rawArgs;
if (sep >= 0) extra = rawArgs.slice(sep + 1);

if (argv.length < 2 || argv[0] === '-h' || argv[0] === '--help') {
  console.log(
    'usage: reduce-js <file.js> <needle> [--appdir DIR] [--env K=V ...] ' +
    '[--timeout S] [--absent] [-- extra bro-headless args]'
  );
  Bronze.exit(2);
}

const src = argv[0];
const needle = argv[1];
const absSrc = Bronze.path.resolve(src);
let appdir = Bronze.path.dirname(absSrc);
const env = { BRO_CODE_CACHE: '0' };
let timeoutSec = 60.0;
let absent = false;

for (let i = 2; i < argv.length; ) {
  const a = argv[i];
  if (a === '--appdir' && i + 1 < argv.length) {
    appdir = argv[i + 1];
    i += 2;
  } else if (a === '--env' && i + 1 < argv.length) {
    const kv = argv[i + 1];
    const eq = kv.indexOf('=');
    const k = eq >= 0 ? kv.slice(0, eq) : kv;
    const v = eq >= 0 ? kv.slice(eq + 1) : '';
    env[k] = v;
    i += 2;
  } else if (a === '--timeout' && i + 1 < argv.length) {
    timeoutSec = parseFloat(argv[i + 1]);
    i += 2;
  } else if (a === '--absent') {
    absent = true;
    i += 1;
  } else {
    console.error(`reduce-js: unknown option ${a}`);
    Bronze.exit(2);
  }
}

const exeExt = Bronze.platform === 'windows' ? '.exe' : '';
const headless =
  Bronze.getenv('BRO_HEADLESS') ||
  Bronze.path.resolve(Bronze.cwd(), 'build-release', 'bro-headless' + exeExt);

if (!Bronze.fs.exists(headless)) {
  console.error(`reduce-js: no bro-headless at ${headless} (set BRO_HEADLESS)`);
  Bronze.exit(2);
}
if (!Bronze.fs.exists(Bronze.path.join(appdir, 'index.html'))) {
  console.error(`reduce-js: ${appdir} has no index.html (pass --appdir)`);
  Bronze.exit(2);
}

const trial = Bronze.path.join(Bronze.path.dirname(absSrc), '_reduce_trial.js');
let runs = 0;

function shows(lines) {
  runs += 1;
  Bronze.fs.writeText(trial, lines.join('\n') + '\n');
  const r = Bronze.exec([headless, ...extra, appdir, trial], {
    env,
    timeoutMs: Math.round(timeoutSec * 1000),
  });
  if (r.timedOut) return false;
  const out = r.stdout + r.stderr;
  return absent ? !out.includes(needle) : out.includes(needle);
}

let lines = Bronze.fs.readText(src).replace(/\r\n/g, '\n').split('\n');
while (lines.length > 0 && !lines[lines.length - 1].trim()) lines.pop();

if (!shows(lines)) {
  console.error('reduce-js: the original file does not show the symptom');
  Bronze.fs.unlink(trial);
  Bronze.exit(1);
}

let n = 2;
while (lines.length >= 2) {
  const chunk = Math.max(1, Math.floor(lines.length / n));
  let shrank = false;
  for (let start = 0; start < lines.length; start += chunk) {
    const cand = lines.slice(0, start).concat(lines.slice(start + chunk));
    if (cand.length > 0 && shows(cand)) {
      lines = cand;
      n = Math.max(n - 1, 2);
      shrank = true;
      break;
    }
  }
  if (!shrank) {
    if (chunk === 1) break;
    n = Math.min(lines.length, n * 2);
    continue;
  }
  console.error(`  ${lines.length} lines (${runs} runs)`);
}

Bronze.fs.unlink(trial);
const ext = Bronze.path.extname(src);
const outPath = (ext ? src.slice(0, -ext.length) : src) + '.min.js';
Bronze.fs.writeText(outPath, lines.join('\n') + '\n');
console.log(`${lines.length} lines after ${runs} runs -> ${outPath}`);
console.log(lines.join('\n'));
