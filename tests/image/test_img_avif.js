// AVIF decodes everywhere an image does: broimage reads the HEIF container
// (its own reader), dav1d decodes the AV1, broimage converts the YUV. The
// fixtures (tests/image/fixtures/avif_*.avif) are ffmpeg/libaom stills of
// known solid colours: 8-bit 4:2:0 and 10-bit 4:2:0 limited range, 12-bit
// 4:4:4 full range BT.709, an alpha auxiliary image, a PQ (HDR) grey, a red |
// blue image and four tiles. Grids, irot/imir, Exif and thumbnail items are
// built here around them (heif_build.js), independently of broimage's reader.

import { readPrimary, writeHeif, irot, imir, ispe, gridData, exifPayload } from './heif_build.js';

const fs = require('fs');
const path = require('path');
const os = require('os');

const FIX = 'tests/image/fixtures/';
const read = (n) => new Uint8Array(fs.readFileSync(path.resolve(FIX + n)));
const px = (img, x, y) => {
  const i = (y * img.width + x) * 4;
  return [img.pixels[i], img.pixels[i + 1], img.pixels[i + 2], img.pixels[i + 3]];
};
const near = (p, want, tol) => want.every((v, k) => Math.abs(p[k] - v) <= tol);
const show = (p) => JSON.stringify(p);

if (bro.image.canDecode('image/avif') !== 'probably') {
  skipTest('this build has no AV1 decoder: ' + bro.image.decodeSupport('image/avif').reason);
} else {
  assert(bro.image.canDecode('avif') === 'probably' && bro.image.canDecode('.AVIF') === 'probably',
         'canDecode takes extensions');
  assert(bro.image.canDecode('image/png') === 'probably' && bro.image.canDecode('image/x-nope') === '',
         'canDecode answers other types too');

  // ── Solid colours through every bit depth and layout ───────────────────────
  // Expected colours are what ffmpeg (libdav1d + swscale, honouring the nclx)
  // decodes from the same files. The 12-bit one was meant as #30C050, but the
  // encode's own RGB->YUV step landed it on [47,164,81].
  const cases = [
    ['avif_8bit_420.avif', 64, 48, [0xc0, 0x30, 0x20]],
    ['avif_10bit_420.avif', 64, 48, [0x20, 0x60, 0xc0]],
    ['avif_12bit_444_full.avif', 48, 32, [47, 164, 81]],
  ];
  for (const [name, w, h, rgb] of cases) {
    const bytes = read(name);
    const probe = bro.image.probeDimensions(bytes);
    assert(probe && probe.width === w && probe.height === h && probe.channels === 3,
           name + ': probeDimensions from ispe: ' + JSON.stringify(probe));
    const img = bro.image.decodeOriented(bytes);
    assert(img.width === w && img.height === h, name + ': decodes ' + img.width + 'x' + img.height + ' ' + (img.error || ''));
    for (const [x, y] of [[0, 0], [w - 1, h - 1], [w >> 1, h >> 1]])
      assert(near(px(img, x, y), [...rgb, 255], 6), name + ' (' + x + ',' + y + ') ' + show(px(img, x, y)) + ' ~ ' + show(rgb));
  }
  const info10 = bro.image.readHeifInfo(read('avif_10bit_420.avif'));
  assert(info10 && info10.mime === 'image/avif' && info10.codec === 'av01' && info10.bitDepth === 10,
         'readHeifInfo: ' + JSON.stringify(info10));
  const info12 = bro.image.readHeifInfo(read('avif_12bit_444_full.avif'));
  assert(info12.bitDepth === 12 && info12.fullRange && info12.matrixCoefficients === 1, 'the 12-bit one is full-range BT.709');

  // ── Alpha ───────────────────────────────────────────────────────────────────
  const alpha = read('avif_alpha.avif');
  assert(bro.image.probeDimensions(alpha).channels === 4, 'an alpha item makes it 4 channels');
  const a = bro.image.decodeOriented(alpha);
  // ffmpeg decodes its colour as [54,145,93] (meant as #40A060, as above).
  assert(a.width === 32 && near(px(a, 16, 16), [54, 145, 93, 128], 6),
         'alpha: straight colour at half alpha ' + show(px(a, 16, 16)));
  assert(bro.image.readHeifInfo(alpha).hasAlpha, 'readHeifInfo says hasAlpha');

  // ── HDR: PQ tone-mapped to SDR ───────────────────────────────────────────
  const pq = read('avif_pq.avif');
  const pqInfo = bro.image.readHeifInfo(pq);
  assert(pqInfo.hdr && pqInfo.transferCharacteristics === 16, 'the PQ fixture is HDR: ' + JSON.stringify(pqInfo));
  const pqImg = bro.image.decodeOriented(pq);
  const g = px(pqImg, 4, 4);
  // ffmpeg's 0x808080 grey as a PQ signal is ~92 nits: below SDR white
  // (203), so it tone-maps to a mid grey, neutral.
  assert(g[0] > 120 && g[0] < 230 && Math.abs(g[0] - g[1]) <= 3 && Math.abs(g[1] - g[2]) <= 3,
         'PQ grey lands as a neutral SDR grey: ' + show(g));

  // ── A grid of four tiles, output 60 x 50 (the tiles overhang) ───────────────
  const tiles = ['red', 'lime', 'blue', 'white'].map((c) => readPrimary(read('avif_tile_' + c + '.avif')));
  const gridFile = writeHeif({
    primary: 1,
    items: [
      { id: 1, type: 'grid', data: gridData(2, 2, 60, 50), props: [ispe(60, 50)] },
      ...tiles.map((t, i) => ({ id: 10 + i, type: t.type, data: t.data, props: t.props, hidden: true })),
    ],
    refs: [{ type: 'dimg', from: 1, to: [10, 11, 12, 13] }],
  });
  const gp = bro.image.probeDimensions(gridFile);
  assert(gp && gp.width === 60 && gp.height === 50, 'a grid probes at its output size: ' + JSON.stringify(gp));
  const gi = bro.image.decodeOriented(gridFile);
  assert(gi.width === 60 && gi.height === 50, 'the grid decodes 60x50 ' + (gi.error || ''));
  const quads = [[5, 5, [255, 0, 0]], [50, 5, [0, 255, 0]], [5, 45, [0, 0, 255]], [55, 45, [255, 255, 255]]];
  for (const [x, y, rgb] of quads)
    assert(near(px(gi, x, y), [...rgb, 255], 8), 'grid tile at ' + x + ',' + y + ': ' + show(px(gi, x, y)));
  const gInfo = bro.image.readHeifInfo(gridFile);
  assert(gInfo.grid && gInfo.gridRows === 2 && gInfo.gridColumns === 2, 'readHeifInfo sees the grid');

  // ── irot / imir: the orientation, as the container says ─────────────────────
  const rb = readPrimary(read('avif_red_blue.avif'));  // 64x32, red left, blue right
  const turned = (props) => writeHeif({ items: [{ id: 1, type: rb.type, data: rb.data, props: [...rb.props, ...props] }] });
  const ccw = turned([irot(1)]);
  assert(bro.image.readExifOrientation(ccw) === 8, 'irot 1 (a quarter turn anti-clockwise) is EXIF 8');
  const stored = bro.image.probeDimensions(ccw);
  assert(stored.width === 64 && stored.height === 32, 'probe gives the stored size');
  const up = bro.image.decodeOriented(ccw);
  assert(up.width === 32 && up.height === 64, 'decodeOriented turns it: ' + up.width + 'x' + up.height);
  assert(near(px(up, 16, 8), [0, 0, 255, 255], 8) && near(px(up, 16, 56), [255, 0, 0, 255], 8),
         'turned anti-clockwise: blue on top, red below');
  const lr = turned([imir(1)]);
  assert(bro.image.readExifOrientation(lr) === 2, 'imir mode 1 mirrors left-right (EXIF 2)');
  const m = bro.image.decodeOriented(lr);
  assert(near(px(m, 4, 16), [0, 0, 255, 255], 8), 'mirrored: blue on the left');
  assert(bro.image.readExifOrientation(turned([imir(0)])) === 4, 'imir mode 0 mirrors top-bottom (EXIF 4)');

  // ── Exif item: readExif reads it; its Orientation tag is ignored ────────────
  const withExif = writeHeif({
    items: [
      { id: 1, type: rb.type, data: rb.data, props: [...rb.props, irot(3)] },
      { id: 2, type: 'Exif', data: exifPayload('AvifCam', 8), props: [], hidden: true },
    ],
    refs: [{ type: 'cdsc', from: 2, to: [1] }],
  });
  const ex = bro.image.readExif(withExif);
  assert(ex && ex.make === 'AvifCam', 'readExif reads the Exif item: ' + JSON.stringify(ex));
  assert(ex.orientation === 6 && bro.image.readExifOrientation(withExif) === 6,
         'orientation is irot 3 (EXIF 6), not the Exif tag (8)');

  // ── A thumbnail item ────────────────────────────────────────────────────────
  const withThumb = writeHeif({
    items: [
      { id: 1, type: rb.type, data: rb.data, props: rb.props },
      { id: 2, type: tiles[1].type, data: tiles[1].data, props: tiles[1].props, hidden: true },
    ],
    refs: [{ type: 'thmb', from: 2, to: [1] }],
  });
  assert(bro.image.readHeifInfo(withThumb).thumbnails.length === 1, 'readHeifInfo lists the thumbnail');
  const th = bro.image.decodeThumbnail(withThumb, 16);
  assert(th && th.width === 32 && near(px(th, 3, 3), [0, 255, 0, 255], 8), 'decodeThumbnail decodes it');
  assert(bro.image.decodeThumbnail(read('avif_8bit_420.avif')) === null, 'no thumbnail item: null');

  // ── A truncated file: still sized, fails with a reason ──────────────────────
  const cut = read('avif_8bit_420.avif').slice(0, 300);
  assert(bro.image.probeDimensions(cut).width === 64, 'a truncated AVIF still probes');
  const bad = bro.image.decodeOriented(cut);
  assert(bad.width === 0 && typeof bad.error === 'string' && bad.error.length > 0, 'and fails with a reason: ' + bad.error);

  // ── <img>, CSS and createImageBitmap ─────────────────────────────────────────
  const dir = path.join(os.tmpdir(), 'bro_img_avif_' + process.pid + '_' + Date.now());
  fs.mkdirSync(dir, { recursive: true });
  const put = (name, bytes) => {
    const f = path.join(dir, name).replace(/\\/g, '/');
    fs.writeFileSync(f, bytes);
    return f;
  };
  const solid = put('solid.avif', read('avif_8bit_420.avif'));
  const rot = put('rot.avif', ccw);
  const alphaFile = put('alpha.avif', alpha);
  document.body.style.margin = '0';
  document.body.style.background = 'rgb(0,0,0)';
  document.body.innerHTML =
      '<img id="solid" src="' + solid + '" style="position:absolute;left:0;top:0">' +
      '<img id="up" src="' + rot + '" style="position:absolute;left:100px;top:0">' +
      '<img id="raw" src="' + rot + '" style="position:absolute;left:200px;top:0;image-orientation:none">' +
      '<div id="bg" style="position:absolute;left:0;top:100px;width:64px;height:48px;background-image:url(\'' + solid + '\')"></div>' +
      '<img id="alpha" src="' + alphaFile + '" style="position:absolute;left:300px;top:0">';
  flush();
  for (const id of ['solid', 'up', 'raw', 'alpha']) await document.getElementById(id).decode();
  flush();
  const s = document.getElementById('solid');
  assert(s.naturalWidth === 64 && s.naturalHeight === 48, '<img> AVIF natural size');
  const p0 = getPixel(10, 10);
  assert(Math.abs(p0.r - 0xc0) <= 8 && Math.abs(p0.g - 0x30) <= 8 && Math.abs(p0.b - 0x20) <= 8, '<img> paints it: ' + show(p0));
  const u = document.getElementById('up');
  assert(u.naturalWidth === 32 && u.naturalHeight === 64, '<img> applies irot: ' + u.naturalWidth + 'x' + u.naturalHeight);
  const top = getPixel(116, 8), bottom = getPixel(116, 56);
  assert(top.b > 200 && top.r < 60 && bottom.r > 200 && bottom.b < 60, '<img> turned: blue on top ' + show(top) + ', red below ' + show(bottom));
  const raw = document.getElementById('raw').getBoundingClientRect();
  assert(Math.round(raw.width) === 64 && Math.round(raw.height) === 32, 'image-orientation:none keeps the stored box');
  const left = getPixel(204, 16), right = getPixel(260, 16);
  assert(left.r > 200 && right.b > 200, 'none paints the stored pixels: ' + show(left) + ' ' + show(right));
  const bg = getPixel(20, 120);
  assert(Math.abs(bg.r - 0xc0) <= 8 && Math.abs(bg.g - 0x30) <= 8, 'a CSS background-image AVIF paints: ' + show(bg));
  const ap = getPixel(316, 16);
  // Half-alpha [54,145,93] over black.
  assert(Math.abs(ap.r - 27) <= 8 && Math.abs(ap.g - 72) <= 8 && Math.abs(ap.b - 46) <= 8,
         '<img> blends the alpha: ' + show(ap));

  const bmp = await createImageBitmap(new Blob([read('avif_10bit_420.avif')], { type: 'image/avif' }));
  assert(bmp.width === 64 && bmp.height === 48, 'createImageBitmap(Blob) decodes an AVIF: ' + bmp.width + 'x' + bmp.height);
  bmp.close();
  fs.rmSync(dir, { recursive: true, force: true });
}
