// IME preedit rendering: the provisional composition text draws with an
// underline. Comparative pixel check — the same text rendered committed
// (shot A) vs as a preedit (shot B) at the same position with the same caret
// differs only by the underline, so shot B must contain measurably more dark
// pixels inside the input's box. ASCII preedit text keeps the check
// font-independent (a romaji composition stage is a real IME state anyway).

const os = require('os');
const path = require('path');
const fs = require('fs');

const root = document.getElementById('root');
root.innerHTML =
    '<input id="u1" type="text" style="width:220px; color:#000; background:#fff">';
flush();

const el = document.getElementById('u1');
const r = el.getBoundingClientRect();
click(r.left + 5, r.top + r.height / 2);

function darkPixelsInRect(file) {
    const img = new Image();
    img.src = file;
    assert(img.naturalWidth > 0, 'screenshot decodes: ' + file);
    const cnv = document.createElement('canvas');
    cnv.width = img.naturalWidth;
    cnv.height = img.naturalHeight;
    const c2 = cnv.getContext('2d');
    c2.drawImage(img, 0, 0);
    const rect = el.getBoundingClientRect();
    const x0 = Math.round(rect.left), y0 = Math.round(rect.top);
    const w = Math.round(rect.width), h = Math.round(rect.height);
    const d = c2.getImageData(x0, y0, w, h).data;
    let n = 0;
    for (let i = 0; i < d.length; i += 4) {
        if (d[i] + d[i + 1] + d[i + 2] < 300) n++;
    }
    return n;
}

// Shot A: committed text, focused, caret at the end.
textInput('hello nihon');
flush();
const shotA = path.join(os.tmpdir(), 'bro_ime_a_' + Date.now() + '.png').replace(/\\/g, '/');
screenshot(shotA);
const darkA = darkPixelsInRect(shotA);

// Back to empty, then the same text as a preedit (composition cursor at the
// end — same caret position as shot A).
keyDown(122 /* z */, 0, 0x0040 /* LCTRL */);
keyUp(122, 0, 0x0040);
assert(el.value === '', 'undo cleared the field, got: ' + JSON.stringify(el.value));
imeCompose('hello nihon');
assert(el.value === 'hello nihon', 'preedit in value, got: ' + JSON.stringify(el.value));
flush();
const shotB = path.join(os.tmpdir(), 'bro_ime_b_' + Date.now() + '.png').replace(/\\/g, '/');
screenshot(shotB);
const darkB = darkPixelsInRect(shotB);

// The underline spans the preedit run (~11 glyphs, well over 40px). Loose
// threshold: the preedit shot needs meaningfully more dark pixels.
assert(darkA > 0, 'committed text renders (dark pixels present), got: ' + darkA);
assert(darkB > darkA + 15,
       'preedit adds an underline: dark(B)=' + darkB + ' vs dark(A)=' + darkA);

imeCancel();
try { fs.unlinkSync(shotA); } catch (e) {}
try { fs.unlinkSync(shotB); } catch (e) {}
