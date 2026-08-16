// <input type=file>: clicking it opens the native picker, and what the user
// chose reads back as real File objects. Headless has no picker to open, so
// setPickedFiles() queues the choice and the click consumes it.

const root = document.getElementById('root');
root.innerHTML = '<input id="f" type="file" accept=".obj,.json">' +
                 '<input id="t" type="text">';
flush();

const f = document.getElementById('f');
const t = document.getElementById('t');

assert(f.files !== null && f.files.length === 0, 'a fresh file input has no files');
assert(t.files === null, 'a non-file input has no FileList at all');

let changes = 0, inputs = 0;
f.addEventListener('change', () => changes++);
f.addEventListener('input', () => inputs++);

// A click with nothing queued is a cancelled pick: no files, no events.
f.click();
assert(f.files.length === 0, 'a cancelled pick selects nothing');
assert(changes === 0 && inputs === 0, 'a cancelled pick fires no events');

// Pick a real file — this test file itself, so the bytes are checkable.
const picked = bro.resolvePath('/app/index.html');
setPickedFiles([picked]);
f.click();

assert(f.files.length === 1, 'one file picked');
assert(changes === 1, 'change fired once');
assert(inputs === 1, 'input fired once');

const file = f.files[0];
assert(file instanceof File, 'the entry is a real File');
assert(file.name === 'index.html', 'the File is named after the path: ' + file.name);
assert(file.size > 0, 'the File has bytes');
assert(file.path === picked, 'the File carries its real path');

// The value reads as a browser reports it: never the real path.
assert(f.value.indexOf('fakepath') >= 0, 'value is the fake path: ' + f.value);
assert(f.value.indexOf('index.html') >= 0, 'value ends in the filename: ' + f.value);

// A second cancelled pick leaves the earlier selection alone, as in a browser.
f.click();
assert(f.files.length === 1, 'a cancelled pick keeps the previous selection');
assert(changes === 1, 'a cancelled pick fires no further change');

// Multiple files.
setPickedFiles([picked, picked]);
f.click();
assert(f.files.length === 2, 'both picked files are in the list');

root.innerHTML = '';
