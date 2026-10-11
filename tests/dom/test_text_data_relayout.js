// A text node's data written in place (data, nodeValue, appendData,
// insertData, deleteData, replaceData, splitText) lays out and paints again.
// These marked nothing, so the text kept the box and the pixels it had until
// something else on the page changed (frameworks update text this way). For
// each mutator: the box it is in measures the new text, and the pixels show
// it; also for text that is a shadow root's own child, which lays out its host.

document.body.style.margin = '0';

// One line of text in a box of its own at a fixed place, black on white.
function makeLine(top, parentOf) {
    const line = document.createElement('div');
    line.style.cssText = `position: absolute; left: 0; top: ${top}px; height: 24px; background: #fff`;
    const span = document.createElement('span');
    span.style.cssText = 'font: 18px sans-serif; color: #000; white-space: pre';
    line.appendChild(span);
    document.body.appendChild(line);
    const t = document.createTextNode('ab');
    (parentOf ? parentOf(span) : span).appendChild(t);
    return { span, t, top };
}
const width = (el) => el.getBoundingClientRect().width;
// Whether anything dark is painted in the line between x0 and x1.
function inked(top, x0, x1) {
    const px = getPixels(x0, top, x1 - x0, 24);
    for (let i = 0; i < px.data.length; i += 4) if (px.data[i] < 128) return true;
    return false;
}

let y = 0;
// Applies `mutate` to a line holding 'ab', whose text then reads `expect`.
function check(name, mutate, expect, parentOf) {
    const L = makeLine(y, parentOf);
    y += 30;
    flush();
    const w0 = width(L.span);
    assert(!inked(L.top, Math.ceil(w0) + 4, 300), `${name}: nothing painted past 'ab' at first`);
    mutate(L.t);
    flush();
    const w1 = width(L.span);
    assert(w1 > w0 + 40, `${name}: the box measures '${expect}' (${w0} -> ${w1})`);
    assert(inked(L.top, Math.ceil(w0) + 4, Math.floor(w1)), `${name}: the new text is painted`);
    // And back, shorter: the box shrinks and the pixels past it clear.
    L.t.deleteData(1, L.t.length - 1);
    flush();
    const w2 = width(L.span);
    assert(w2 < w0, `${name}: shortened, the box shrinks (${w1} -> ${w2})`);
    assert(!inked(L.top, Math.ceil(w0) + 4, Math.floor(w1)), `${name}: shortened, the old text is gone`);
}

check('data =', (t) => { t.data = 'abcdefghijkl'; }, 'abcdefghijkl');
check('nodeValue =', (t) => { t.nodeValue = 'abcdefghijkl'; }, 'abcdefghijkl');
check('appendData', (t) => t.appendData('cdefghijkl'), 'abcdefghijkl');
check('insertData', (t) => t.insertData(1, 'XXXXXXXX'), 'aXXXXXXXXb');
check('replaceData', (t) => t.replaceData(1, 1, 'WWWWWWWW'), 'aWWWWWWWW');
check('shadow root text', (t) => { t.data = 'abcdefghijkl'; }, 'abcdefghijkl',
      (span) => span.attachShadow({ mode: 'open' }));

// splitText: the node keeps the head; the tail is a new node beside it.
{
    const L = makeLine(y, null);
    y += 30;
    L.t.data = 'abcdefghijkl';
    flush();
    const w0 = width(L.span);
    const tail = L.t.splitText(2);
    tail.remove();
    flush();
    assert(width(L.span) < w0 - 40, `splitText: the head alone measures 'ab' (${w0} -> ${width(L.span)})`);
    assert(!inked(L.top, Math.ceil(width(L.span)) + 4, Math.floor(w0)), 'splitText: the split-off text is gone');
}

// Going empty and back restyles: :empty matches it.
const style = document.createElement('style');
style.textContent = '.e:empty { display: none } .e { display: inline-block; width: 30px; height: 10px }';
document.head.appendChild(style);
const e = document.createElement('span');
e.className = 'e';
const et = document.createTextNode('x');
e.appendChild(et);
document.body.appendChild(e);
flush();
assert(e.getBoundingClientRect().width === 30, 'shown with text');
et.data = '';
flush();
assert(e.getBoundingClientRect().width === 0, 'its text emptied, :empty hides it');
et.data = 'y';
flush();
assert(e.getBoundingClientRect().width === 30, 'text again, shown again');
console.log('text data relayout: checked');
