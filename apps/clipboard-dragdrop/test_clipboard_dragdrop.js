// Integration test: clipboard and drag-drop functionality

flush();

// --- Part 1: Clipboard paste into input ---

var clipSrc = document.getElementById('clip-src');
var clipDst = document.getElementById('clip-dst');
assert(clipSrc !== null, 'clip-src exists');
assert(clipDst !== null, 'clip-dst exists');

// Focus the target input by clicking it
var dstRect = clipDst.getBoundingClientRect();
click(dstRect.left + 10, dstRect.top + 10);
flush();

// Paste text
paste('Hello from headless!');

assert(clipDst.value === 'Hello from headless!',
    'paste should insert text into focused input, got: "' + clipDst.value + '"');
console.log('PASS: paste into input');

// --- Part 2: Copy from input ---

// Focus the source input
var srcRect = clipSrc.getBoundingClientRect();
click(srcRect.left + 10, srcRect.top + 10);
flush();

var copied = copy();
assert(copied === 'Hello from Bro!',
    'copy should return input value, got: "' + copied + '"');

// Source should still have its value
assert(clipSrc.value === 'Hello from Bro!', 'copy should not clear source');
console.log('PASS: copy from input');

// --- Part 3: Cut from input ---

var cutText = cut();
assert(cutText === 'Hello from Bro!',
    'cut should return input value, got: "' + cutText + '"');
assert(clipSrc.value === '',
    'cut should clear the input, got: "' + clipSrc.value + '"');
console.log('PASS: cut from input');

// --- Part 4: Clipboard events fire ---

var pasteEvents = [];
clipDst.addEventListener('paste', function(e) {
    pasteEvents.push({
        type: e.type,
        text: e.clipboardData ? e.clipboardData.getData('text/plain') : ''
    });
});

click(dstRect.left + 10, dstRect.top + 10);
flush();
paste('Event test');

assert(pasteEvents.length === 1, 'paste event should fire');
assert(pasteEvents[0].text === 'Event test',
    'paste event should carry text, got: "' + pasteEvents[0].text + '"');
console.log('PASS: clipboard events');

// --- Part 5: Textarea paste ---

var textarea = document.getElementById('textarea');
var taRect = textarea.getBoundingClientRect();
click(taRect.left + 10, taRect.top + 10);
flush();

// Type content into textarea first (sets the value attribute)
textarea.value = 'Test content';
flush();

// Copy from textarea
var taCopied = copy();
assert(taCopied === 'Test content',
    'copy should return textarea value, got: "' + taCopied + '"');

// Paste new content
paste(' appended');
assert(textarea.value.indexOf('appended') !== -1,
    'textarea should have pasted text');
console.log('PASS: textarea clipboard');

// --- Part 6: File drop ---

var fileDropZone = document.getElementById('file-drop');
assert(fileDropZone !== null, 'file-drop zone exists');

var dropEvents = [];
fileDropZone.addEventListener('dragenter', function(e) {
    dropEvents.push('dragenter');
});
fileDropZone.addEventListener('drop', function(e) {
    e.preventDefault();
    var files = e.dataTransfer ? e.dataTransfer.files : [];
    var names = [];
    for (var i = 0; i < files.length; i++) {
        names.push(files[i].name);
    }
    dropEvents.push('drop:' + names.join(','));
});

var fdRect = fileDropZone.getBoundingClientRect();
var fdx = fdRect.left + fdRect.width / 2;
var fdy = fdRect.top + fdRect.height / 2;

dropFiles(fdx, fdy, ['C:/Users/test/photo.jpg', 'C:/Users/test/document.pdf']);

assert(dropEvents.length >= 2, 'should have dragenter + drop events, got: ' + JSON.stringify(dropEvents));
// dragenter fires once per file, drop fires once per file too
var hasEnter = dropEvents.some(function(e) { return e === 'dragenter'; });
assert(hasEnter, 'dragenter should fire');
var hasDrop = dropEvents.some(function(e) { return e.indexOf('drop:') === 0; });
assert(hasDrop, 'drop should fire with file names');
console.log('PASS: file drop');

// --- Part 7: Text drop ---

var textDropZone = document.getElementById('text-drop');
assert(textDropZone !== null, 'text-drop zone exists');

var textDropEvents = [];
textDropZone.addEventListener('dragenter', function(e) {
    textDropEvents.push('dragenter');
});
textDropZone.addEventListener('drop', function(e) {
    e.preventDefault();
    var text = e.dataTransfer ? e.dataTransfer.getData('text/plain') : '';
    textDropEvents.push('drop:' + text);
});

var tdRect = textDropZone.getBoundingClientRect();
var tdx = tdRect.left + tdRect.width / 2;
var tdy = tdRect.top + tdRect.height / 2;

dropText(tdx, tdy, 'Dropped text content');

assert(textDropEvents.length >= 2, 'should have dragenter + drop, got: ' + JSON.stringify(textDropEvents));
var hasTextDrop = textDropEvents.some(function(e) { return e === 'drop:Dropped text content'; });
assert(hasTextDrop, 'drop should contain the dropped text, got: ' + JSON.stringify(textDropEvents));
console.log('PASS: text drop');

// --- Part 8: Event log populated ---

var logEl = document.getElementById('log');
var logChildren = logEl.childNodes.length;
// The app's own event handlers should have logged events
assert(logChildren > 1, 'event log should have entries from clipboard/drop events, got ' + logChildren);
console.log('PASS: event log populated (' + logChildren + ' entries)');

console.log('ALL TESTS PASSED');
