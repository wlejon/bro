// An <img> settles with exactly one `load` or `error`, however its src was set
// (markup via innerHTML, setAttribute, the `src` property) — and a file that
// exists but is not an image bro decodes (an .exe) is an `error`, as a missing
// one is. Markup and setAttribute images used to fire neither.
//
// And .ico decodes (Windows app icons): an icon holding a 16x16 BMP and a
// larger PNG loads as the PNG, the biggest entry, with its pixels.

const fs = require('fs');
const path = require('path');

const root = document.getElementById('root');
const stamp = Date.now();
const made = [];
function tmp(name) {
  const p = path.join(bro.appDir, 'tmp_imgev_' + stamp + '_' + name);
  made.push(p);
  return p;
}

// A 40x24 PNG of a solid colour, made by screenshotting a div.
root.innerHTML = '<div style="width:40px;height:24px;background:rgb(10,200,30)"></div>';
flush();
const pngPath = tmp('src.png');
screenshot(pngPath, 0, 0, 40, 24);
const png = new Uint8Array(fs.readFileSync(pngPath));
root.innerHTML = '';

// An ICO: a 16x16 32-bit BMP entry (red) and the PNG entry.
function u16(v) { return [v & 255, (v >> 8) & 255]; }
function u32(v) { return [v & 255, (v >> 8) & 255, (v >> 16) & 255, (v >>> 24) & 255]; }
function bmpEntry(w, h) {
  const b = [].concat(u32(40), u32(w), u32(h * 2), u16(1), u16(32), u32(0), u32(0),
                      u32(0), u32(0), u32(0), u32(0));
  for (let i = 0; i < w * h; i++) b.push(0, 0, 255, 255);          // BGRA red
  const stride = Math.ceil(w / 32) * 4;
  for (let i = 0; i < stride * h; i++) b.push(0);                  // AND mask: opaque
  return new Uint8Array(b);
}
function ico(entries) {
  const head = [].concat(u16(0), u16(1), u16(entries.length));
  let off = 6 + 16 * entries.length;
  for (const e of entries) {
    head.push(e.w >= 256 ? 0 : e.w, e.h >= 256 ? 0 : e.h, 0, 0);
    head.push(...u16(1), ...u16(32), ...u32(e.bytes.length), ...u32(off));
    off += e.bytes.length;
  }
  const out = new Uint8Array(off);
  out.set(head, 0);
  let at = 6 + 16 * entries.length;
  for (const e of entries) { out.set(e.bytes, at); at += e.bytes.length; }
  return out;
}
const icoPath = tmp('app.ico');
writeFile(icoPath, ico([{ w: 16, h: 16, bytes: bmpEntry(16, 16) }, { w: 40, h: 24, bytes: png }]));
const bmpOnlyPath = tmp('small.ico');
writeFile(bmpOnlyPath, ico([{ w: 16, h: 16, bytes: bmpEntry(16, 16) }]));
// Exists, but no image decoder takes it: a PE header and padding.
const exePath = tmp('tool.exe');
writeFile(exePath, new Uint8Array([0x4d, 0x5a, 0x90, 0, 3, 0, 0, 0, 4, 0, 0, 0, 0xff, 0xff, 0, 0]
                                    .concat(new Array(240).fill(0))));
// A .ico in name only.
const fakeIcoPath = tmp('fake.ico');
writeFile(fakeIcoPath, 'not an icon at all');
const missing = path.join(bro.appDir, 'tmp_imgev_' + stamp + '_missing.png');

try {
  const rel = (p) => path.basename(p);

  // Each case: how the src is set, and what must fire.
  const cases = [
    { name: 'markup png', src: rel(pngPath), want: 'load' },
    { name: 'markup ico', src: rel(icoPath), want: 'load', size: [40, 24] },
    { name: 'markup bmp-in-ico', src: rel(bmpOnlyPath), want: 'load', size: [16, 16] },
    { name: 'markup exe', src: rel(exePath), want: 'error' },
    { name: 'markup fake ico', src: rel(fakeIcoPath), want: 'error' },
    { name: 'markup missing', src: rel(missing), want: 'error' },
  ];

  // 1. Markup, with listeners attached after parsing (before the task runs).
  root.innerHTML = cases.map((c, i) => '<img id="m' + i + '" src="' + c.src + '">').join('');
  const fired = cases.map(() => []);
  cases.forEach((c, i) => {
    const el = document.getElementById('m' + i);
    el.addEventListener('load', () => fired[i].push('load'));
    el.addEventListener('error', () => fired[i].push('error'));
  });
  flush();
  advanceTime(16);
  cases.forEach((c, i) => {
    assert(fired[i].length === 1 && fired[i][0] === c.want,
           c.name + ': fires exactly one ' + c.want + ', got [' + fired[i].join(',') + ']');
    if (c.size) {
      const el = document.getElementById('m' + i);
      assert(el.naturalWidth === c.size[0] && el.naturalHeight === c.size[1],
             c.name + ': natural size ' + c.size.join('x') + ' (the largest entry), got ' +
             el.naturalWidth + 'x' + el.naturalHeight);
    }
  });

  // The icon paints: the PNG entry's green, not the BMP entry's red.
  const icoEl = document.getElementById('m1');
  const r = icoEl.getBoundingClientRect();
  assert(r.width === 40 && r.height === 24, 'the icon lays out at 40x24, got ' + r.width + 'x' + r.height);
  const p = getPixel(Math.round(r.x + 20), Math.round(r.y + 12));
  assert(p.g > 150 && p.r < 80, 'the icon paints its PNG entry, got rgb(' + p.r + ',' + p.g + ',' + p.b + ')');

  // 2. setAttribute on an existing image, then the `src` property: one event
  //    for each change, never a second from the layout walk.
  root.innerHTML = '';
  const img = document.createElement('img');
  root.appendChild(img);
  const seen = [];
  img.addEventListener('load', () => seen.push('load'));
  img.addEventListener('error', () => seen.push('error'));
  img.setAttribute('src', rel(exePath));
  flush(); advanceTime(16);
  assert(seen.join(',') === 'error', 'setAttribute to an .exe: one error, got [' + seen.join(',') + ']');
  img.setAttribute('src', rel(icoPath));
  flush(); advanceTime(16);
  assert(seen.join(',') === 'error,load', 'setAttribute to an .ico: then one load, got [' + seen.join(',') + ']');
  img.src = rel(fakeIcoPath);
  flush(); advanceTime(16);
  assert(seen.join(',') === 'error,load,error', 'src = a fake .ico: one error, got [' + seen.join(',') + ']');
  img.src = rel(icoPath);
  flush(); advanceTime(16);
  assert(seen.join(',') === 'error,load,error,load', 'src = an .ico: one load, got [' + seen.join(',') + ']');
  assert(img.naturalWidth === 40, 'the src property decodes the icon too, got ' + img.naturalWidth);

  // 3. new Image() decodes an icon.
  const helper = new Image();
  let helperEvent = '';
  helper.onload = () => { helperEvent = 'load'; };
  helper.onerror = () => { helperEvent = 'error'; };
  helper.src = rel(icoPath);
  advanceTime(16);
  assert(helperEvent === 'load' && helper.width === 40 && helper.height === 24,
         'new Image() loads the icon at 40x24, got ' + helperEvent + ' ' + helper.width + 'x' + helper.height);
} finally {
  root.innerHTML = '';
  for (const p of made) { try { fs.unlinkSync(p); } catch (e) {} }
}
