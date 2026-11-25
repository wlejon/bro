// Make machine-level text safe to print: debugger output, disassembly, perf
// reports, hex dumps.
//
//   perf report --stdio ... | bronze run scripts/safeview.js -- --max 60
//   objdump -d f.o | safeview --map perf-1234.map --grep 'cmp|j[a-z]+ '
//   safeview --stats trace.txt

const ADDR_RE = /0x[0-9a-fA-F]{5,}|\b(?=[0-9a-fA-F]*[0-9])[0-9a-fA-F]{8,}\b/g;
const FLOAT_RE = /(\d+\.\d+(?:[eE][-+]?\d+)?)/;
const SMALL_HEX_RE = /\b0x([0-9a-fA-F]{1,4})\b/g;
const BYTE_COLUMN_RE = /(?:\b[0-9a-fA-F]{2}\b[ \t]){3,}/g;
const CXX_TAIL_RE = /(<[^<>]*>|\([^()]*\))/g;
const WORD_RE = /[A-Za-z_][A-Za-z0-9_:.]{2,}/g;

function stripCxx(text) {
  let prev = null;
  while (prev !== text) {
    prev = text;
    text = text.replace(CXX_TAIL_RE, '');
  }
  return text;
}

function shortName(name) {
  return stripCxx(name).trim().slice(0, 80);
}

function bisectRight(arr, x) {
  let lo = 0;
  let hi = arr.length;
  while (lo < hi) {
    const mid = (lo + hi) >>> 1;
    if (arr[mid] <= x) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

class Labels {
  constructor(mapPath) {
    this.starts = [];
    this.syms = [];
    if (mapPath) {
      const rows = [];
      for (const line of Bronze.fs.readText(mapPath).replace(/\r\n/g, '\n').split('\n')) {
        const m = /^\s*([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+(.*)$/.exec(line);
        if (!m) continue;
        try {
          rows.push([BigInt('0x' + m[1]), BigInt('0x' + m[2]), m[3].trim()]);
        } catch (_) {}
      }
      rows.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
      this.starts = rows.map(r => r[0]);
      this.syms = rows;
    }
    this.seen = new Map();
    this.order = [];
  }

  of(addr) {
    if (this.seen.has(addr)) return this.seen.get(addr);
    let label = null;
    const i = bisectRight(this.starts, addr) - 1;
    if (i >= 0) {
      const [start, size, name] = this.syms[i];
      const effSize = size > 1n ? size : 1n;
      if (addr < start + effSize) {
        label = `${shortName(name)}+${(addr - start).toString()}`;
      }
    }
    if (label === null) {
      const j = bisectRight(this.order, addr) - 1;
      if (j >= 0 && addr - this.order[j] < 4096n) {
        label = `${this.seen.get(this.order[j])}+${(addr - this.order[j]).toString()}`;
      } else {
        label = `@${this.order.length + 1}`;
        this.order.splice(j + 1, 0, addr);
      }
    }
    this.seen.set(addr, label);
    return label;
  }
}

function cleanLine(rawLine, labels, width) {
  let line = '';
  const end = rawLine.endsWith('\r') ? rawLine.length - 1 : rawLine.length;
  for (let i = 0; i < end; i++) {
    const code = rawLine.charCodeAt(i);
    line += (code >= 32 && code < 127) || code === 9 ? rawLine[i] : '?';
  }
  line = line.replace(BYTE_COLUMN_RE, '');
  const parts = line.split(FLOAT_RE);
  for (let i = 0; i < parts.length; i += 2) {
    parts[i] = parts[i].replace(ADDR_RE, m => {
      const hex = m.startsWith('0x') || m.startsWith('0X') ? m.slice(2) : m;
      return labels.of(BigInt('0x' + hex));
    });
  }
  line = parts.join('');
  line = line.replace(SMALL_HEX_RE, (_m, hex) => String(parseInt(hex, 16)));
  if (line.includes('::') && line.length > width) line = stripCxx(line);
  return line.slice(0, width);
}

const args = Bronze.args;
let filePath = '';
let mapPath = '';
let labelsPath = '';
let grepPat = '';
let maxLines = 80;
let width = 160;
let stats = false;

for (let i = 0; i < args.length; i++) {
  const a = args[i];
  if (a === '--map' && i + 1 < args.length) mapPath = args[++i];
  else if (a.startsWith('--map=')) mapPath = a.slice(6);
  else if (a === '--labels' && i + 1 < args.length) labelsPath = args[++i];
  else if (a.startsWith('--labels=')) labelsPath = a.slice(9);
  else if (a === '--grep' && i + 1 < args.length) grepPat = args[++i];
  else if (a.startsWith('--grep=')) grepPat = a.slice(7);
  else if (a === '--max' && i + 1 < args.length) maxLines = parseInt(args[++i], 10);
  else if (a.startsWith('--max=')) maxLines = parseInt(a.slice(6), 10);
  else if (a === '--width' && i + 1 < args.length) width = parseInt(args[++i], 10);
  else if (a.startsWith('--width=')) width = parseInt(a.slice(8), 10);
  else if (a === '--stats') stats = true;
  else if (a === '-h' || a === '--help') {
    console.log('usage: safeview [file] [--map FILE] [--labels FILE] [--grep REGEX] [--max N] [--width N] [--stats]');
    Bronze.exit(0);
  } else if (!filePath) {
    filePath = a;
  }
}

const labels = new Labels(mapPath);
const keep = grepPat ? new RegExp(grepPat) : null;
const rawBytes = filePath ? Bronze.fs.readBytes(filePath) : Bronze.stdin.readBytes();

let shown = 0;
let total = 0;
const words = new Map();

let lineStart = 0;
for (let i = 0; i <= rawBytes.length; i++) {
  if (i === rawBytes.length && lineStart === rawBytes.length) break;
  if (i < rawBytes.length && rawBytes[i] !== 10) continue;
  let chunk = '';
  for (let j = lineStart; j < i; j++) chunk += String.fromCharCode(rawBytes[j]);
  lineStart = i + 1;
  const line = cleanLine(chunk, labels, width);
  if (keep && !keep.test(line)) continue;
  total += 1;
  if (stats) {
    WORD_RE.lastIndex = 0;
    let m;
    while ((m = WORD_RE.exec(line)) !== null) {
      words.set(m[0], (words.get(m[0]) || 0) + 1);
    }
  } else if (shown < maxLines) {
    console.log(line);
    shown += 1;
  }
}

if (stats) {
  console.log(`lines ${total}, distinct addresses ${labels.seen.size}`);
  const ranked = Array.from(words.entries()).sort((a, b) => b[1] - a[1]).slice(0, maxLines);
  for (const [w, n] of ranked) console.log(`${String(n).padStart(8)}  ${w}`);
} else if (total > shown) {
  console.log(`... ${total - shown} more lines`);
}

if (labelsPath) {
  const entries = Array.from(labels.seen.entries()).sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
  const out = entries.map(([addr, label]) => `${label} ${addr.toString(16)}`).join('\n') + (entries.length ? '\n' : '');
  Bronze.fs.writeText(labelsPath, out);
}
