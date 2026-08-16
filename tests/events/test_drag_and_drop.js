// HTML5 drag and drop between elements: the `draggable` attribute and the
// dragstart → drag → dragenter → dragover → drop → dragend sequence, with one
// DataTransfer living for the whole gesture.
//
// This is how a page lets the user rearrange its own things (reorder a list,
// reparent a tree node). Distinct from the OS file drop, which comes from
// outside the window — see tests/dom/test_filereader.js and dropFiles().

const root = document.getElementById('root');
root.innerHTML =
    '<div id="src" draggable="true" style="position:absolute;left:0;top:0;width:80px;height:40px">drag</div>' +
    '<div id="dst" style="position:absolute;left:0;top:100px;width:120px;height:60px">drop</div>' +
    '<div id="plain" style="position:absolute;left:200px;top:0;width:80px;height:40px">plain</div>';
flush();

const src = document.getElementById('src');
const dst = document.getElementById('dst');
const plain = document.getElementById('plain');

let log = [];
function note(s) { if (log[log.length - 1] !== s) log.push(s); }

src.addEventListener('dragstart', e => {
    note('dragstart');
    e.dataTransfer.setData('text/plain', 'payload-42');
    e.dataTransfer.effectAllowed = 'move';
});
src.addEventListener('drag', () => note('drag'));
src.addEventListener('dragend', () => note('dragend'));
dst.addEventListener('dragenter', e => { note('dragenter'); e.preventDefault(); });
dst.addEventListener('dragover', e => { note('dragover'); e.preventDefault(); });
dst.addEventListener('dragleave', () => note('dragleave'));

let dropped = null;
dst.addEventListener('drop', e => {
    e.preventDefault();
    note('drop');
    dropped = {
        text: e.dataTransfer.getData('text/plain'),
        legacy: e.dataTransfer.getData('Text'),      // the old spelling
        types: e.dataTransfer.types.join(','),
        effect: e.dataTransfer.effectAllowed,
        offsetY: e.offsetY,
    };
});

function dragFrom(a, b) {
    const from = a.getBoundingClientRect(), to = b.getBoundingClientRect();
    mouseDown(from.left + 10, from.top + 10);
    mouseMove(from.left + 20, from.top + 20);       // past the threshold
    mouseMove(to.left + 30, to.top + 20);
    mouseMove(to.left + 40, to.top + 30);
    mouseUp(to.left + 40, to.top + 30);
    flush();
}

dragFrom(src, dst);

assert(log.join(' ') === 'dragstart drag dragenter dragover drag dragover drop dragend',
       'the whole sequence fires in order (got: ' + log.join(' ') + ')');

assert(dropped !== null, 'the drop handler ran');
assert(dropped.text === 'payload-42', 'the data the source set survives to the drop');
assert(dropped.legacy === 'payload-42', '"Text" is an alias for text/plain');
assert(dropped.types === 'text/plain', 'types lists what was set: ' + dropped.types);
assert(dropped.effect === 'move', 'effectAllowed survives the gesture');
assert(dropped.offsetY > 0, 'offsetY is target-relative, which drop handlers branch on');

// A target that never calls preventDefault refuses the drop.
log = [];
let plainDropped = false;
plain.addEventListener('drop', () => { plainDropped = true; });
dragFrom(src, plain);
assert(!plainDropped, 'no drop on a target that did not accept it');
assert(log.indexOf('dragend') >= 0, 'dragend still fires when nothing accepted');

// A press that never moves far enough is a click, not a drag.
log = [];
let clicks = 0;
src.addEventListener('click', () => clicks++);
const s = src.getBoundingClientRect();
mouseDown(s.left + 10, s.top + 10);
mouseMove(s.left + 11, s.top + 11);
mouseUp(s.left + 11, s.top + 11);
flush();
assert(log.length === 0, 'a short press starts no drag (got: ' + log.join(' ') + ')');
assert(clicks === 1, 'and is still a click');

// A completed drag is not also a click.
clicks = 0;
dragFrom(src, dst);
assert(clicks === 0, 'a drag does not end in a click');

// draggable reflects as a property, which is how pages usually set it.
const made = document.createElement('div');
assert(made.draggable === false, 'a plain div is not draggable');
made.draggable = true;
assert(made.getAttribute('draggable') === 'true', 'the property writes the attribute');
assert(made.draggable === true, 'and reads back true');

// A div is not a form control: .value is an ordinary expando on it, with the
// value's own type intact — list widgets stash ids there.
made.value = 17;
assert(made.value === 17, '.value on a div keeps its type');
assert(made.getAttribute('value') === null, '.value on a div is not an attribute');

root.innerHTML = '';
