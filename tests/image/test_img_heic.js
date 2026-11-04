// HEIC decodes through the system's decoder only — bro carries no HEVC
// decoder: WIC on Windows (with the Microsoft Store's HEVC Video
// Extensions), ImageIO on macOS, the system's libheif on Linux (opened at
// run time when installed). broimage's own HEIF reader still sizes the
// image and reads its orientation everywhere, and a decode that cannot
// happen says why; the test skips there.
//
// The fixtures (tests/image/fixtures): heic_quadrants.heic was made by WIC's
// HEIF encoder (broimage's test_heif_os, BROIMAGE_HEIC_FIXTURE_OUT),
// heic_thumb.heic by libheif's heif-enc (-t 16, so a 16x12 thumbnail item,
// coded 64x64 with a clean aperture): both 64x48, red top-left, green
// bottom-left, blue on the right.

import { readPrimary, writeHeif, irot, imir } from './heif_build.js';

const fs = require('fs');
const path = require('path');
const os = require('os');

const heic = new Uint8Array(fs.readFileSync(path.resolve('tests/image/fixtures/heic_quadrants.heic')));
const px = (img, x, y) => {
  const i = (y * img.width + x) * 4;
  return [img.pixels[i], img.pixels[i + 1], img.pixels[i + 2]];
};
const near = (p, want, tol) => want.every((v, k) => Math.abs(p[k] - v) <= tol);
const show = (p) => JSON.stringify(p);

// Everywhere: the container answers without a decoder.
const probe = bro.image.probeDimensions(heic);
assert(probe && probe.width === 64 && probe.height === 48, 'HEIC probes from ispe: ' + JSON.stringify(probe));
const info = bro.image.readHeifInfo(heic);
assert(info && info.mime === 'image/heic' && info.codec === 'hvc1', 'readHeifInfo: ' + JSON.stringify(info));
const p = readPrimary(heic);
const turned = writeHeif({ brand: 'heic', compat: ['mif1', 'heic'],
                           items: [{ id: 1, type: p.type, data: p.data, props: [...p.props, irot(1), imir(1)] }] });
assert(bro.image.readExifOrientation(turned) === 7, 'irot 1 + imir 1 is EXIF 7, with or without a decoder');

const support = bro.image.decodeSupport('image/heic');
assert((bro.image.canDecode('image/heic') === 'probably') === support.supported, 'canDecode agrees with decodeSupport');
if (!support.supported) {
  // The reason is what the decode reports too.
  assert(support.reason.length > 0, 'unsupported HEIC says why');
  const img = bro.image.decodeOriented(heic);
  assert(img.width === 0 && img.error === support.reason, 'a HEIC decode fails with that reason: ' + img.error);
  if (process.platform === 'win32') assert(/HEVC Video Extensions/.test(support.reason), 'Windows names the Store extension');
  if (process.platform === 'linux') assert(/install libheif/.test(support.reason), 'Linux says to install libheif: ' + support.reason);
  skipTest('no HEIC decoder here: ' + support.reason);
} else {
  const img = bro.image.decodeOriented(heic);
  assert(img.width === 64 && img.height === 48, 'HEIC decodes ' + img.width + 'x' + img.height + ' ' + (img.error || ''));
  assert(near(px(img, 16, 12), [255, 0, 0], 32) && near(px(img, 16, 36), [0, 255, 0], 32) &&
         near(px(img, 48, 24), [0, 0, 255], 32), 'HEIC colours: ' + show(px(img, 16, 12)) + show(px(img, 16, 36)) + show(px(img, 48, 24)));

  // Turned by the container: as stored from probe, upright from decodeOriented.
  const up = bro.image.decodeOriented(turned);
  // EXIF 7 (transverse): stored (x, y) lands at (H-1-y, W-1-x); red (top-left) goes bottom-right.
  assert(up.width === 48 && up.height === 64, 'the turned HEIC decodes upright: ' + up.width + 'x' + up.height);
  assert(near(px(up, 36, 48), [255, 0, 0], 32) && near(px(up, 12, 48), [0, 255, 0], 32) &&
         near(px(up, 24, 16), [0, 0, 255], 32), 'upright HEIC colours ' + show(px(up, 36, 48)) + show(px(up, 12, 48)));

  // A box no bigger than the thumbnail item is served from it, as a photo
  // browser's grid asks; a bigger one decodes the image (its clean aperture
  // taken, 64x48 of the coded 64x64).
  const thumbFile = new Uint8Array(fs.readFileSync(path.resolve('tests/image/fixtures/heic_thumb.heic')));
  const small = bro.image.decodeScaled(thumbFile, { width: 16, height: 16 });
  assert(small.path === 'heif-thumbnail' && small.width === 16 && small.height === 12,
         'a 16 px box uses the thumbnail item: ' + JSON.stringify({ path: small.path, w: small.width, h: small.height, e: small.error }));
  assert(near(px(small, 4, 3), [255, 0, 0], 48) && near(px(small, 12, 6), [0, 0, 255], 48), 'thumbnail colours ' + show(px(small, 4, 3)));
  const big = bro.image.decodeScaled(thumbFile, { width: 64, height: 64 });
  assert(big.path === 'full' && big.sourceWidth === 64 && big.sourceHeight === 48 && big.width === 64 && big.height === 48,
         'a 64 px box decodes the image: ' + JSON.stringify({ path: big.path, w: big.width, h: big.height, e: big.error }));
  // 48: WIC decodes this file with the BT.709 matrix where its colr says
  // BT.601 (green comes back 0,216,0); libheif within a few levels.
  assert(near(px(big, 16, 12), [255, 0, 0], 48) && near(px(big, 16, 36), [0, 255, 0], 48),
         'full decode colours ' + show(px(big, 16, 12)) + show(px(big, 16, 36)));

  // <img>
  const dir = path.join(os.tmpdir(), 'bro_img_heic_' + process.pid + '_' + Date.now());
  fs.mkdirSync(dir, { recursive: true });
  const file = path.join(dir, 'q.heic').replace(/\\/g, '/');
  fs.writeFileSync(file, heic);
  document.body.style.margin = '0';
  document.body.innerHTML = '<img id="h" src="' + file + '" style="position:absolute;left:0;top:0">';
  flush();
  const el = document.getElementById('h');
  await el.decode();
  flush();
  assert(el.naturalWidth === 64 && el.naturalHeight === 48, '<img> HEIC natural size');
  const c = getPixel(48, 24);
  assert(c.b > 200 && c.r < 60, '<img> paints the HEIC: ' + show(c));
  fs.rmSync(dir, { recursive: true, force: true });
}
