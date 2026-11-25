// Compare two raw RGBA readPixels dumps (bottom-up rows, as WebGL returns them).
//
//   bronze run bench/realworld/compare_shots.js -- a.rgba b.rgba --size 1280x720 [--tol 8] [--out prefix]
//
// Also imported directly by rw.js (`compareShots`). Writes standard PNGs with
// pure-JS RFC-1950/1951 stored deflate blocks (zero external dependencies).

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    t[i] = c >>> 0;
  }
  return t;
})();

function crc32Bytes(buf, start = 0, len = buf.length - start) {
  let c = 0xffffffff;
  for (let i = start; i < start + len; i++) {
    c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  }
  return (c ^ 0xffffffff) >>> 0;
}

function adler32(buf) {
  let a = 1;
  let b = 0;
  for (let i = 0; i < buf.length; i++) {
    a = (a + buf[i]) % 65521;
    b = (b + a) % 65521;
  }
  return ((b << 16) | a) >>> 0;
}

function zlibStore(raw) {
  const maxBlock = 65535;
  const numBlocks = Math.max(1, Math.ceil(raw.length / maxBlock));
  const out = new Uint8Array(2 + raw.length + numBlocks * 5 + 4);
  let pos = 0;
  out[pos++] = 0x78;
  out[pos++] = 0x01;
  let offset = 0;
  for (let b = 0; b < numBlocks; b++) {
    const len = Math.min(maxBlock, raw.length - offset);
    const isFinal = b === numBlocks - 1 ? 1 : 0;
    out[pos++] = isFinal;
    out[pos++] = len & 0xff;
    out[pos++] = (len >>> 8) & 0xff;
    const nlen = (~len) & 0xffff;
    out[pos++] = nlen & 0xff;
    out[pos++] = (nlen >>> 8) & 0xff;
    if (len > 0) {
      out.set(raw.subarray(offset, offset + len), pos);
      pos += len;
      offset += len;
    }
  }
  const adl = adler32(raw);
  out[pos++] = (adl >>> 24) & 0xff;
  out[pos++] = (adl >>> 16) & 0xff;
  out[pos++] = (adl >>> 8) & 0xff;
  out[pos++] = adl & 0xff;
  return out;
}

function writeU32BE(buf, off, val) {
  buf[off] = (val >>> 24) & 0xff;
  buf[off + 1] = (val >>> 16) & 0xff;
  buf[off + 2] = (val >>> 8) & 0xff;
  buf[off + 3] = val & 0xff;
}

function makeChunk(typeStr, data) {
  const out = new Uint8Array(12 + data.length);
  writeU32BE(out, 0, data.length);
  for (let i = 0; i < 4; i++) out[4 + i] = typeStr.charCodeAt(i);
  out.set(data, 8);
  const crc = crc32Bytes(out, 4, 4 + data.length);
  writeU32BE(out, 8 + data.length, crc);
  return out;
}

export function writePngRgb(path, w, h, rgbRows) {
  const rowStride = 1 + w * 3;
  const raw = new Uint8Array(h * rowStride);
  for (let y = 0; y < h; y++) {
    raw[y * rowStride] = 0;
    raw.set(rgbRows[y], y * rowStride + 1);
  }
  const ihdr = new Uint8Array(13);
  writeU32BE(ihdr, 0, w);
  writeU32BE(ihdr, 4, h);
  ihdr[8] = 8;
  ihdr[9] = 2;
  const sig = new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10]);
  const c1 = makeChunk('IHDR', ihdr);
  const c2 = makeChunk('IDAT', zlibStore(raw));
  const c3 = makeChunk('IEND', new Uint8Array(0));
  const png = new Uint8Array(sig.length + c1.length + c2.length + c3.length);
  let p = 0;
  for (const part of [sig, c1, c2, c3]) {
    png.set(part, p);
    p += part.length;
  }
  Bronze.fs.writeBytes(path, png);
}

function uprightRgb(buf, w, h) {
  const rows = [];
  for (let y = h - 1; y >= 0; y--) {
    const row = new Uint8Array(w * 3);
    const base = y * w * 4;
    for (let x = 0; x < w; x++) {
      const si = base + x * 4;
      const di = x * 3;
      row[di] = buf[si];
      row[di + 1] = buf[si + 1];
      row[di + 2] = buf[si + 2];
    }
    rows.push(row);
  }
  return rows;
}

export function compareShots(pathA, pathB, opts = {}) {
  const sizeStr = opts.size || '1280x720';
  const tol = opts.tol ?? 8;
  const outPrefix = opts.out || '';
  const includeAlpha = !!opts.alpha;
  const [w, h] = sizeStr.split('x').map(x => parseInt(x, 10));
  const A = Bronze.fs.readBytes(pathA);
  const B = Bronze.fs.readBytes(pathB);
  if (A.length !== w * h * 4 || B.length !== w * h * 4) {
    console.error(`size mismatch: ${A.length} / ${B.length} bytes, expected ${w * h * 4}`);
    Bronze.exit(1);
  }
  const nch = includeAlpha ? 4 : 3;
  let maxd = 0;
  let total = 0;
  let over = 0;
  let differ = 0;
  const diffRows = [];
  for (let y = h - 1; y >= 0; y--) {
    const row = new Uint8Array(w * 3);
    const base = y * w * 4;
    for (let x = 0; x < w; x++) {
      const i = base + x * 4;
      let d = 0;
      for (let c = 0; c < nch; c++) {
        const dc = Math.abs(A[i + c] - B[i + c]);
        total += dc;
        if (dc > d) d = dc;
      }
      if (d) differ += 1;
      if (d > maxd) maxd = d;
      if (d > tol) {
        over += 1;
        row[x * 3] = 255;
        row[x * 3 + 1] = Math.min(255, d);
        row[x * 3 + 2] = 0;
      } else {
        const v = Math.min(255, d * 4);
        row[x * 3] = v;
        row[x * 3 + 1] = v;
        row[x * 3 + 2] = v;
      }
    }
    diffRows.push(row);
  }
  const px = w * h;
  console.log(
    `pixels ${px}  differing ${differ} (${(100 * differ / px).toFixed(3)}%)  max channel diff ${maxd}  ` +
    `mean abs diff ${(total / (px * nch)).toFixed(4)}  over tol ${tol}: ${over} (${(100 * over / px).toFixed(3)}%)`
  );
  if (outPrefix) {
    writePngRgb(outPrefix + '_a.png', w, h, uprightRgb(A, w, h));
    writePngRgb(outPrefix + '_b.png', w, h, uprightRgb(B, w, h));
    writePngRgb(outPrefix + '_diff.png', w, h, diffRows);
    console.log('wrote', outPrefix + '_{a,b,diff}.png');
  }
}

export function runCompareCli(argv) {
  let a = '';
  let b = '';
  let size = '1280x720';
  let tol = 8;
  let out = '';
  let alpha = false;
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--size' && i + 1 < argv.length) size = argv[++i];
    else if (arg.startsWith('--size=')) size = arg.slice(7);
    else if (arg === '--tol' && i + 1 < argv.length) tol = parseInt(argv[++i], 10);
    else if (arg.startsWith('--tol=')) tol = parseInt(arg.slice(6), 10);
    else if (arg === '--out' && i + 1 < argv.length) out = argv[++i];
    else if (arg.startsWith('--out=')) out = arg.slice(6);
    else if (arg === '--alpha') alpha = true;
    else if (!a) a = arg;
    else if (!b) b = arg;
  }
  if (!a || !b) {
    console.error('usage: compare_shots <a.rgba> <b.rgba> [--size 1280x720] [--tol 8] [--out prefix] [--alpha]');
    Bronze.exit(2);
  }
  compareShots(a, b, { size, tol, out, alpha });
}
