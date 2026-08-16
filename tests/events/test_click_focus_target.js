// A press focuses the nearest FOCUSABLE ancestor, not whatever pixel it landed on.
//
// The engine used to make the hit target the activeElement outright. That reads
// as harmless until a widget focuses its own element from a mousedown handler:
// CodeMirror parks a hidden <textarea> and reads keystrokes out of it, and the
// press that was supposed to put the caret in the editor handed focus to the
// <span class="cm-keyword"> under the pointer instead, so the editor accepted
// no typing at all. Per HTML only form controls, links with href, elements with
// tabindex and editing hosts take focus from a click; everything else takes it
// away and leaves it on the body.

const root = document.getElementById('root');
root.innerHTML =
    '<div id="plain" style="position:absolute;left:0;top:0;width:200px;height:40px">' +
    '  <span id="inner">text</span></div>' +
    '<input id="field" style="position:absolute;left:0;top:50px;width:200px;height:24px">' +
    '<button id="btn" style="position:absolute;left:0;top:90px;width:200px;height:30px">' +
    '  <span id="label">press</span></button>' +
    '<a id="link" href="#x" style="position:absolute;left:0;top:130px;width:200px;height:24px">link</a>' +
    '<div id="tabbable" tabindex="0" style="position:absolute;left:0;top:170px;width:200px;height:30px">' +
    '  <span id="tabinner">tabbable</span></div>' +
    '<div id="editable" contenteditable="true" style="position:absolute;left:0;top:210px;width:200px;height:30px">' +
    'edit me</div>';
flush();

function centreOf(id) {
    const b = document.getElementById(id).getBoundingClientRect();
    return [(b.left + b.width / 2) | 0, (b.top + b.height / 2) | 0];
}
function clickOn(id) { const c = centreOf(id); click(c[0], c[1]); flush(); }

const field = document.getElementById('field');

// --- a focusable target takes focus ---------------------------------------
clickOn('field');
assert(document.activeElement === field, 'clicking an input focuses it');

// --- ordinary content does not ---------------------------------------------
clickOn('inner');
assert(document.activeElement !== document.getElementById('inner'),
       'a bare <span> does not become activeElement');
assert(document.activeElement !== document.getElementById('plain'),
       'nor does its bare <div> parent');
assert(document.activeElement === document.body,
       'focus lands on the body, got <' +
       (document.activeElement ? document.activeElement.tagName : 'null') + '>');

// --- and it still took focus AWAY from the field ---------------------------
let blurred = false;
field.addEventListener('blur', function () { blurred = true; });
clickOn('field');
assert(document.activeElement === field, 'field focused again');
clickOn('inner');
assert(blurred, 'clicking away from the field blurred it');

// --- the nearest focusable ANCESTOR wins ----------------------------------
clickOn('label');
assert(document.activeElement === document.getElementById('btn'),
       'a click on a <span> inside a <button> focuses the button, got <' +
       document.activeElement.tagName + '>');

clickOn('tabinner');
assert(document.activeElement === document.getElementById('tabbable'),
       'tabindex makes an ordinary div focusable for its subtree');

clickOn('link');
assert(document.activeElement === document.getElementById('link'),
       'an <a href> is focusable');

clickOn('editable');
assert(document.activeElement === document.getElementById('editable'),
       'an editing host is focusable');

// --- the widget idiom this exists for -------------------------------------
// A hidden field focused from a mousedown handler on non-focusable content
// must still hold focus once the press finishes.
const hidden = document.createElement('input');
hidden.id = 'hidden';
hidden.style.cssText = 'position:absolute;left:-1000px;top:0;width:10px;height:10px';
root.appendChild(hidden);
document.getElementById('plain').addEventListener('mousedown', function () {
    hidden.focus();
});
flush();
clickOn('inner');
assert(document.activeElement === hidden,
       'focus taken in a mousedown handler survives the press, got <' +
       document.activeElement.tagName + ' id=' + document.activeElement.id + '>');

root.innerHTML = '';
console.log('PASS: click focus goes to the nearest focusable ancestor');
