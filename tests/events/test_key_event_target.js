// keydown / keyup target the focused element, whatever it is.
//
// The engine used to aim a key at <body> unless a text control claimed it, and
// at whatever node the document Selection sat in when the Selection moved the
// caret. So a focused field saw its own Enter and Backspace but not its arrow
// keys, its space bar, or any letter — the events went past it to the body.
// Anything listening on its own element for its own keys missed them.
//
// Also covers window.focus() / window.blur(), which did not exist at all: a
// library calling window.focus() mid-handler took a TypeError and lost the rest
// of the handler with it.

const root = document.getElementById('root');
root.innerHTML =
    '<input id="field" style="position:absolute;left:0;top:0;width:200px;height:24px">' +
    '<div id="tabbable" tabindex="0" style="position:absolute;left:0;top:40px;width:200px;height:30px">x</div>';
flush();

const field = document.getElementById('field');
const tabbable = document.getElementById('tabbable');

const SDLK_a = 0x61, SDLK_LEFT = 0x40000050, SDLK_SPACE = 0x20, SDLK_RETURN = 0x0d;

function centre(el) {
    const b = el.getBoundingClientRect();
    return [(b.left + b.width / 2) | 0, (b.top + b.height / 2) | 0];
}

// --- a focused input gets its own keys -------------------------------------
const onField = [];
field.addEventListener('keydown', function (e) { onField.push('down:' + e.key); });
field.addEventListener('keyup', function (e) { onField.push('up:' + e.key); });

let c = centre(field);
click(c[0], c[1]);
flush();
assert(document.activeElement === field, 'field focused');

[SDLK_a, SDLK_LEFT, SDLK_SPACE, SDLK_RETURN].forEach(function (k) { keyDown(k); keyUp(k); });
flush();

['down:a', 'down:ArrowLeft', 'down: ', 'down:Enter'].forEach(function (want) {
    assert(onField.indexOf(want) !== -1,
           want + ' reached the focused input, got ' + JSON.stringify(onField));
});
assert(onField.indexOf('up:a') !== -1, 'keyup reaches it too');

// --- a focused non-control element gets them as well ------------------------
const onDiv = [];
tabbable.addEventListener('keydown', function (e) { onDiv.push(e.key); });
c = centre(tabbable);
click(c[0], c[1]);
flush();
assert(document.activeElement === tabbable, 'the tabindex div is focused');

keyDown(SDLK_a); keyUp(SDLK_a);
keyDown(SDLK_LEFT); keyUp(SDLK_LEFT);
flush();
assert(onDiv.indexOf('a') !== -1 && onDiv.indexOf('ArrowLeft') !== -1,
       'a focused div receives keys, got ' + JSON.stringify(onDiv));

// --- with nothing focused they fall to the body ----------------------------
tabbable.blur();
flush();
const onBody = [];
document.body.addEventListener('keydown', function (e) { onBody.push(e.target.tagName); });
keyDown(SDLK_a); keyUp(SDLK_a);
flush();
assert(onBody.length >= 1 && onBody[0] === 'BODY',
       'with nothing focused the key targets the body, got ' + JSON.stringify(onBody));

// --- window.focus / window.blur exist and are harmless ---------------------
assert(typeof window.focus === 'function', 'window.focus is a function');
assert(typeof window.blur === 'function', 'window.blur is a function');
window.focus();
window.blur();
assert(document.activeElement === document.body,
       'window.focus() does not disturb DOM focus');

root.innerHTML = '';
console.log('PASS: key events target the focused element');
