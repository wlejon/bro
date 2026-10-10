// A small HEIF reader/writer for the AVIF/HEIC tests, written from ISO/IEC
// 23008-12 apart from broimage's reader: it takes the coded image and its
// properties out of a one-item file (what ffmpeg and WIC write), and builds
// new files around them — grids, irot/imir, Exif and thumbnail items.

const u32 = (b, i) => ((b[i] << 24) | (b[i + 1] << 16) | (b[i + 2] << 8) | b[i + 3]) >>> 0;
const u16 = (b, i) => (b[i] << 8) | b[i + 1];
const type = (b, i) => String.fromCharCode(b[i], b[i + 1], b[i + 2], b[i + 3]);

function children(b, start, end) {
  const out = [];
  for (let p = start; p + 8 <= end;) {
    let size = u32(b, p);
    if (size === 0) size = end - p;
    out.push({ type: type(b, p + 4), start: p, body: p + 8, end: p + size });
    p += size;
  }
  return out;
}

function uN(b, i, n) {
  let v = 0;
  for (let k = 0; k < n; k++) v = v * 256 + b[i + k];
  return v;
}

/** The primary item of a single-image HEIF: { type, data, props: [box bytes] }. */
export function readPrimary(b) {
  const top = children(b, 0, b.length);
  const meta = top.find((x) => x.type === 'meta');
  const kids = children(b, meta.body + 4, meta.end);
  const pitm = kids.find((x) => x.type === 'pitm');
  const primary = b[pitm.body] === 0 ? u16(b, pitm.body + 4) : u32(b, pitm.body + 4);
  // iinf: the primary's type.
  const iinf = kids.find((x) => x.type === 'iinf');
  const iinfV = b[iinf.body];
  let itemType = null;
  for (const e of children(b, iinf.body + 4 + (iinfV === 0 ? 2 : 4), iinf.end)) {
    const v = b[e.body];
    const id = v === 2 ? u16(b, e.body + 4) : u32(b, e.body + 4);
    const at = e.body + 4 + (v === 2 ? 2 : 4) + 2;
    if (id === primary) itemType = type(b, at);
  }
  // iloc: its extents.
  const iloc = kids.find((x) => x.type === 'iloc');
  const v = b[iloc.body];
  let p = iloc.body + 4;
  const os = b[p] >> 4, ls = b[p] & 15, bs = b[p + 1] >> 4, is = v ? b[p + 1] & 15 : 0;
  p += 2;
  const count = v < 2 ? u16(b, p) : u32(b, p);
  p += v < 2 ? 2 : 4;
  let data = null;
  for (let i = 0; i < count; i++) {
    const id = v < 2 ? u16(b, p) : u32(b, p);
    p += v < 2 ? 2 : 4;
    const method = v ? u16(b, p) & 15 : 0;
    if (v) p += 2;
    p += 2;  // data_reference_index
    const base = uN(b, p, bs);
    p += bs;
    const n = u16(b, p);
    p += 2;
    const parts = [];
    for (let e = 0; e < n; e++) {
      p += is;
      const off = uN(b, p, os);
      p += os;
      const len = uN(b, p, ls);
      p += ls;
      parts.push([base + off, len]);
    }
    if (id === primary) {
      if (method !== 0) throw new Error('heif_build: only file-offset items');
      const total = parts.reduce((s, [, l]) => s + l, 0);
      data = new Uint8Array(total);
      let w = 0;
      for (const [o, l] of parts) { data.set(b.subarray(o, o + l), w); w += l; }
    }
  }
  // iprp: the properties associated with it.
  const iprp = kids.find((x) => x.type === 'iprp');
  const pk = children(b, iprp.body, iprp.end);
  const ipco = pk.find((x) => x.type === 'ipco');
  const props = children(b, ipco.body, ipco.end).map((x) => b.slice(x.start, x.end));
  const ipma = pk.find((x) => x.type === 'ipma');
  const mv = b[ipma.body], wide = b[ipma.body + 3] & 1;
  p = ipma.body + 4;
  const entries = u32(b, p);
  p += 4;
  const mine = [];
  for (let i = 0; i < entries; i++) {
    const id = mv < 1 ? u16(b, p) : u32(b, p);
    p += mv < 1 ? 2 : 4;
    const n = b[p++];
    for (let a = 0; a < n; a++) {
      const idx = wide ? u16(b, p) & 0x7fff : b[p] & 0x7f;
      p += wide ? 2 : 1;
      if (id === primary) mine.push(props[idx - 1]);
    }
  }
  return { type: itemType, data, props: mine };
}

// ---- Writing ----------------------------------------------------------------

const be16 = (n) => [(n >> 8) & 255, n & 255];
const be32 = (n) => [(n >>> 24) & 255, (n >> 16) & 255, (n >> 8) & 255, n & 255];
const ascii = (s) => [...s].map((c) => c.charCodeAt(0));

export function box(t, body) {
  return [...be32(body.length + 8), ...ascii(t), ...body];
}
export function fullbox(t, version, flags, body) {
  return box(t, [version, (flags >> 16) & 255, (flags >> 8) & 255, flags & 255, ...body]);
}
export const irot = (a) => box('irot', [a & 3]);
export const imir = (mode) => box('imir', [mode & 1]);
export const ispe = (w, h) => fullbox('ispe', 0, 0, [...be32(w), ...be32(h)]);
export const auxC = (urn) => fullbox('auxC', 0, 0, [...ascii(urn), 0]);

/** An Exif item payload: offset, "Exif\0\0", a big-endian TIFF with Make and Orientation. */
export function exifPayload(make, orientation) {
  const t = [0x4d, 0x4d, 0, 42, ...be32(8), ...be16(2)];
  t.push(...be16(0x010f), ...be16(2), ...be32(make.length + 1), ...be32(38));
  t.push(...be16(0x0112), ...be16(3), ...be32(1), ...be16(orientation), 0, 0);
  t.push(...be32(0));
  t.push(...ascii(make), 0);
  return [...be32(6), ...ascii('Exif'), 0, 0, ...t];
}

/**
 * A HEIF file. items: [{ id, type, data (bytes), props: [box bytes], hidden }],
 * refs: [{ type, from, to: [ids] }]. Item data goes in mdat (iloc v1, method 0).
 */
export function writeHeif({ brand = 'avif', compat = ['mif1', 'miaf'], primary = 1, items, refs = [] }) {
  const out = box('ftyp', [...ascii(brand), 0, 0, 0, 0, ...ascii(brand), ...compat.flatMap(ascii)]);
  const hdlr = fullbox('hdlr', 0, 0, [0, 0, 0, 0, ...ascii('pict'), ...new Array(12).fill(0), 0]);
  const pitm = fullbox('pitm', 0, 0, be16(primary));
  const iinf = fullbox('iinf', 0, 0, [...be16(items.length),
    ...items.flatMap((it) => fullbox('infe', 2, it.hidden ? 1 : 0, [...be16(it.id), 0, 0, ...ascii(it.type), 0]))]);
  const iref = refs.length ? fullbox('iref', 0, 0, refs.flatMap((r) =>
    box(r.type, [...be16(r.from), ...be16(r.to.length), ...r.to.flatMap(be16)]))) : [];
  let index = 0;
  const ipco = box('ipco', items.flatMap((it) => it.props.flatMap((p) => [...p])));
  const ipma = fullbox('ipma', 0, 0, [...be32(items.length), ...items.flatMap((it) =>
    [...be16(it.id), it.props.length, ...it.props.map(() => 0x80 | ++index)])]);
  const iprp = box('iprp', [...ipco, ...ipma]);
  const iloc = (dataStart) => {
    let off = dataStart;
    return fullbox('iloc', 1, 0, [0x44, 0x00, ...be16(items.length), ...items.flatMap((it) => {
      const at = off;
      off += it.data.length;
      return [...be16(it.id), 0, 0, 0, 0, ...be16(1), ...be32(at), ...be32(it.data.length)];
    })]);
  };
  const meta = (dataStart) => fullbox('meta', 0, 0, [...hdlr, ...pitm, ...iloc(dataStart), ...iinf, ...iref, ...iprp]);
  const dataStart = out.length + meta(0).length + 8;
  const body = [];
  for (const it of items) body.push(...it.data);
  return new Uint8Array([...out, ...meta(dataStart), ...box('mdat', body)]);
}

/** A grid's item data (16-bit sizes). */
export function gridData(rows, cols, w, h) {
  return [0, 0, rows - 1, cols - 1, ...be16(w), ...be16(h)];
}
