// Contenteditable IME preedit rendering: the provisional composition text
// draws with a thin underline (same visual as the controls' preedit).
// Comparative pixel check, mirroring test_ime_underline.js: the same text
// rendered committed (shot A) vs as a preedit (shot B) at the same position
// differs only by the underline, so shot B must contain measurably more dark
// pixels inside the host's box. ASCII preedit text keeps the check
// font-independent (a romaji composition stage is a real IME state anyway).

const os = require('os');
const path = require('path');
const fs = require('fs');

const root = document.getElementById('root');
root.innerHTML =
    '<div id="ed" contenteditable="true" style="width:260px;height:40px;' +
    'color:#000;background:#fff;font-size:16px"></div>';
flush();

const ed = document.getElementById('ed');
const r = ed.getBoundingClientRect();
click(r.left + 5, r.top + 12);
flush();

function darkPixelsInRect(file) {
    const img = new Image();
    img.src = file;
    assert(img.naturalWidth > 0, 'screenshot decodes: ' + file);
    const cnv = document.createElement('canvas');
    cnv.width = img.naturalWidth;
    cnv.height = img.naturalHeight;
    const c2 = cnv.getContext('2d');
    c2.drawImage(img, 0, 0);
    const rect = ed.getBoundingClientRect();
    const x0 = Math.round(rect.left), y0 = Math.round(rect.top);
    const w = Math.round(rect.width), h = Math.round(rect.height);
    const d = c2.getImageData(x0, y0, w, h).data;
    let n = 0;
    for (let i = 0; i < d.length; i += 4) {
        if (d[i] + d[i + 1] + d[i + 2] < 300) n++;
    }
    return n;
}

// Shot A: committed text, caret at the end.
window.getSelection().collapse(ed, 0);
textInput('hello nihon');
flush();
assert(ed.textContent === 'hello nihon', 'typed text landed, got: ' + JSON.stringify(ed.textContent));
const shotA = path.join(os.tmpdir(), 'bro_ime_ce_a_' + Date.now() + '.png').replace(/\\/g, '/');
screenshot(shotA);
const darkA = darkPixelsInRect(shotA);

// Back to empty, then the same text as a preedit (composition cursor at the
// end — same caret position as shot A).
ed.textContent = '';
flush();
window.getSelection().collapse(ed, 0);
imeCompose('hello nihon');
assert(ed.textContent === 'hello nihon', 'preedit in textContent, got: ' + JSON.stringify(ed.textContent));
flush();
const shotB = path.join(os.tmpdir(), 'bro_ime_ce_b_' + Date.now() + '.png').replace(/\\/g, '/');
screenshot(shotB);
const darkB = darkPixelsInRect(shotB);

// The underline spans the preedit run (~11 glyphs, well over 40px). Loose
// threshold: the preedit shot needs meaningfully more dark pixels.
assert(darkA > 0, 'committed text renders (dark pixels present), got: ' + darkA);
assert(darkB > darkA + 15,
       'contenteditable preedit adds an underline: dark(B)=' + darkB + ' vs dark(A)=' + darkA);

imeCancel();
assert(ed.textContent === '' && ed.childNodes.length === 0,
       'cancel after the pixel check restores the empty host');
try { fs.unlinkSync(shotA); } catch (e) {}
try { fs.unlinkSync(shotB); } catch (e) {}
