// toDataURL and toBlob on a canvas no context was ever taken from encode its
// bitmap, transparent black at the canvas's size, as Chromium does; only a
// zero-size canvas gives "data:," and a null blob. bro answered "data:," and
// null for any canvas without a context, so URL.createObjectURL(blob) threw.

const c = document.createElement('canvas');
c.width = 40;
c.height = 30;
const url = c.toDataURL();
assert(url.startsWith('data:image/png;base64,'), 'toDataURL encodes a PNG, got ' + url.slice(0, 30));

let blob = 'pending';
c.toBlob((b) => { blob = b; });
for (let i = 0; i < 20 && blob === 'pending'; i++) advanceTime(16);
assert(blob instanceof Blob && blob.type === 'image/png' && blob.size > 0, 'toBlob gives a PNG blob');

let size = null;
const img = new Image();
img.onload = () => { size = img.naturalWidth + 'x' + img.naturalHeight; };
img.src = URL.createObjectURL(blob);
for (let i = 0; i < 100 && size === null; i++) { sleep(10); advanceTime(16); }
assert(size === '40x30', 'the PNG decodes at the canvas size, got ' + size);

const g = document.createElement('canvas');
g.width = 0;
assert(g.toDataURL() === 'data:,', 'a zero-size canvas is "data:,"');
let nothing = 'pending';
g.toBlob((b) => { nothing = b; });
for (let i = 0; i < 20 && nothing === 'pending'; i++) advanceTime(16);
assert(nothing === null, 'and its blob is null');
