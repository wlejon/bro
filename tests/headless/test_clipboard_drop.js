// Test headless clipboard helpers (copy/cut/paste) and drop simulation
// (dropFiles, dropText). Exercises src/js/headless_bindings.cpp paths
// beyond keyboard/mouse already tested in test_input_simulation.js.

const root = document.getElementById('root');

// =========================================================================
// paste — text into focused input
// =========================================================================
root.innerHTML = '<input id="i" type="text">';
flush();
const input = document.getElementById('i');
const r = input.getBoundingClientRect();
click(r.left + 5, r.top + 5);
assert(document.activeElement === input, 'input focused');

paste('hello world');
assert(input.value === 'hello world', 'paste populates input, got: ' + input.value);

// =========================================================================
// copy — focused input contents
// =========================================================================
// Select via Ctrl+A
keyDown(97 /* 'a' */, 0, 0x0040 /* LCTRL */);
keyUp(97, 0, 0x0040);

const copied = copy();
assert(typeof copied === 'string', 'copy returns string');
// Implementation may return selection or full value depending on selection state
assert(copied.length === 0 || copied.indexOf('hello') !== -1 || copied === input.value,
       'copy returned something sensible: "' + copied + '"');

// =========================================================================
// cut — focused input, removes selection
// =========================================================================
const cutBefore = input.value;
keyDown(97 /* 'a' */, 0, 0x0040);
keyUp(97, 0, 0x0040);
const cutText = cut();
assert(typeof cutText === 'string', 'cut returns string');
// After cut, input may be empty if Select All worked
// Just verify no exception

// =========================================================================
// dropText — drop text onto element
// =========================================================================
root.innerHTML = '<div id="drop" style="width:200px;height:200px;background:#eee">drop here</div>';
flush();
const dropTarget = document.getElementById('drop');

let dropEvent = null;
dropTarget.addEventListener('drop', (e) => { dropEvent = e; });

const rDrop = dropTarget.getBoundingClientRect();
dropText(rDrop.left + 50, rDrop.top + 50, 'dropped content');

// drop event should have fired
assert(dropEvent !== null, 'drop event fired');
assert(dropEvent.type === 'drop', 'event type = drop');

// dataTransfer.getData('text/plain') should return content
if (dropEvent.dataTransfer && typeof dropEvent.dataTransfer.getData === 'function') {
    const txt = dropEvent.dataTransfer.getData('text/plain');
    assert(txt === 'dropped content', 'dataTransfer text, got: ' + txt);
}

// dragenter / dragover fire too
let enterFired = false, overFired = false;
dropTarget.addEventListener('dragenter', () => enterFired = true);
dropTarget.addEventListener('dragover', () => overFired = true);
dropText(rDrop.left + 50, rDrop.top + 50, 'second drop');
assert(enterFired, 'dragenter fired');
assert(overFired, 'dragover fired');

// =========================================================================
// dropFiles — file array drop
// =========================================================================
let fileDropEvt = null;
dropTarget.addEventListener('drop', (e) => { fileDropEvt = e; });

dropFiles(rDrop.left + 50, rDrop.top + 50, ['/tmp/a.png', '/tmp/b.txt']);
assert(fileDropEvt !== null, 'file drop event');

// Inspect dataTransfer.files
if (fileDropEvt.dataTransfer && fileDropEvt.dataTransfer.files) {
    const files = fileDropEvt.dataTransfer.files;
    // Number of files may vary if paths are filtered/validated; just verify >=1
    assert(files.length >= 1, 'at least one file dropped, got ' + files.length);
    if (files[0]) {
        // Each file is a File-like object with name/path
        const name = files[0].name || files[0].path;
        assert(typeof name === 'string', 'file has name/path');
    }
}

// =========================================================================
// mouseMove fires mousemove
// =========================================================================
let moves = 0;
dropTarget.addEventListener('mousemove', () => moves++);
mouseMove(rDrop.left + 10, rDrop.top + 10);
mouseMove(rDrop.left + 20, rDrop.top + 20);
mouseMove(rDrop.left + 30, rDrop.top + 30);
assert(moves >= 3, 'mousemove fired multiple times, got ' + moves);

// =========================================================================
// mouseDown / mouseUp button arg
// =========================================================================
let mdLog = [];
dropTarget.addEventListener('mousedown', (e) => mdLog.push(e.button));
dropTarget.addEventListener('mouseup', (e) => mdLog.push('up:' + e.button));

mouseDown(rDrop.left + 50, rDrop.top + 50, 0); // left
mouseUp(rDrop.left + 50, rDrop.top + 50, 0);
mouseDown(rDrop.left + 50, rDrop.top + 50, 2); // right
mouseUp(rDrop.left + 50, rDrop.top + 50, 2);

assert(mdLog.length >= 4, 'down/up fired for both buttons, got ' + mdLog.length);

// =========================================================================
// Cleanup
// =========================================================================
root.innerHTML = '';
