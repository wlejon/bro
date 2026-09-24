// What a primary-button press does to the document selection.
//
//  - A press on a block with no text puts the caret in THAT block. The text
//    hit test used to search the whole document, so the caret landed in the
//    nearest paragraph anywhere (possibly a contenteditable one).
//  - A press inside a <button>'s child leaves the selection alone, as a press
//    on the button itself does. Only the hit target's own tag was checked, so
//    toolbar markup (<button><b>B</b></button>) collapsed the selection into
//    the <b>.
//  - A press on a <canvas> starts no text selection, and text hit testing
//    skips pointer-events:none text: double-clicking a canvas used to
//    word-select an overlay toast beside it.
//  - A press whose mousedown was default-prevented changes no selection, so
//    a canvas drag the page handles itself does not select text.

const root = document.getElementById('root');
const sel = getSelection();
const mid = (el) => {
    const r = el.getBoundingClientRect();
    return { x: r.left + r.width / 2, y: r.top + r.height / 2, r };
};
function press(x, y) {
    mouseDown(x, y);
    mouseUp(x, y);
    advanceTime(20);
    flush();
}

// --- a text-less block ---------------------------------------------------
{
    root.innerHTML =
        '<p id="other" style="font:20px Arial">some other text</p>' +
        '<div id="plain" style="height:40px"></div>' +
        '<p style="font:20px Arial">plain text below</p>';
    flush();
    const other = document.getElementById('other');
    const plain = document.getElementById('plain');
    sel.collapse(other.firstChild, 2);
    const r = plain.getBoundingClientRect();
    press(r.left + 20, r.top + 20);
    assert(sel.rangeCount === 1 && sel.anchorNode === plain,
        'caret goes into the pressed empty block (anchor ' +
        (sel.anchorNode && sel.anchorNode.nodeName) + ')');
    assert(String(sel) === '', 'nothing selected');
}

// --- inside a <button>'s child -----------------------------------------------
{
    root.innerHTML =
        '<p id="t" style="font:20px Arial">some plain text</p>' +
        '<button id="bold"><b id="bb">Bold</b></button>';
    flush();
    const t = document.getElementById('t').firstChild;
    sel.setBaseAndExtent(t, 0, t, 4);
    const c = mid(document.getElementById('bb'));
    press(c.x, c.y);
    assert(String(sel) === 'some', 'selection survives a press on <b> inside <button> (got "' +
        String(sel) + '")');
    assert(sel.anchorNode === t, 'still anchored in the paragraph');
}

// --- canvas double-click next to pointer-events:none text -------------------
{
    root.innerHTML =
        '<div style="position:relative;font:20px Arial">' +
        '<canvas id="cv" width="300" height="200"></canvas>' +
        '<div id="toast" style="position:absolute;left:10px;top:205px;pointer-events:none">CAT  +15</div>' +
        '</div>';
    flush();
    sel.removeAllRanges();
    const c = mid(document.getElementById('cv'));
    click(c.x, c.r.bottom - 5);
    click(c.x, c.r.bottom - 5);
    advanceTime(20);
    flush();
    assert(String(sel) === '', 'double-clicking the canvas selects nothing (got "' + String(sel) + '")');

    // Pressing ON the pointer-events:none text goes through it, too.
    const toast = mid(document.getElementById('toast'));
    click(toast.x, toast.y);
    click(toast.x, toast.y);
    advanceTime(20);
    flush();
    assert(String(sel) === '', 'the pointer-events:none toast is not word-selected (got "' +
        String(sel) + '")');
}

// --- a drag whose mousedown was cancelled -------------------------------------
{
    root.innerHTML =
        '<div style="font:20px Arial">' +
        '<div id="pad" style="height:200px;width:400px"></div>' +
        '<p>visible after</p></div>';
    flush();
    const pad = document.getElementById('pad');
    pad.addEventListener('mousedown', (e) => e.preventDefault());
    sel.removeAllRanges();
    const r = pad.getBoundingClientRect();
    mouseDown(r.left + 10, r.top + 150);
    mouseMove(r.left + 200, r.top + 180);
    mouseMove(r.left + 390, r.bottom + 15);
    mouseUp(r.left + 390, r.bottom + 15);
    advanceTime(20);
    flush();
    assert(String(sel) === '', 'a drag from a cancelled mousedown selects nothing (got "' +
        String(sel) + '")');
    assert(sel.rangeCount === 0, 'and leaves the (empty) selection as it was');
}

// --- and a canvas drag without preventDefault starts none either ------------
{
    root.innerHTML =
        '<div hidden><span>hidden label</span></div>' +
        '<canvas id="cv2" width="400" height="200"></canvas>' +
        '<p style="font:20px Arial">visible after</p>';
    flush();
    sel.removeAllRanges();
    const r = document.getElementById('cv2').getBoundingClientRect();
    mouseDown(r.left + 10, r.top + 150);
    mouseMove(r.left + 200, r.top + 190);
    mouseMove(r.left + 390, r.bottom + 15);
    mouseUp(r.left + 390, r.bottom + 15);
    advanceTime(20);
    flush();
    assert(String(sel) === '', 'a drag from a canvas selects nothing (got "' + String(sel) + '")');
}

console.log('test_selection_press: done');
