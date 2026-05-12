// Test Blob construction and methods.

assert(typeof Blob === 'function', 'Blob exists');

const b1 = new Blob(['hello, ', 'world'], { type: 'text/plain' });
assert(b1.size === 12, 'size string parts: ' + b1.size);
assert(b1.type === 'text/plain', 'type: ' + b1.type);

// From Uint8Array
const u = new Uint8Array([72, 105]); // "Hi"
const b2 = new Blob([u]);
assert(b2.size === 2, 'size from Uint8Array: ' + b2.size);
assert(b2.type === '', 'default type empty: "' + b2.type + '"');

// Mixed
const b3 = new Blob(['A', new Uint8Array([66]), 'C']);
assert(b3.size === 3, 'mixed size: ' + b3.size);

// .text() returns Promise<string>
let textResult;
b1.text().then(t => { textResult = t; });
flush();
advanceTime(10);
flush();
assert(textResult === 'hello, world', '.text(): ' + textResult);

// .arrayBuffer() returns Promise<ArrayBuffer>
let abResult;
b2.arrayBuffer().then(ab => { abResult = ab; });
flush();
advanceTime(10);
flush();
assert(abResult instanceof ArrayBuffer, 'arrayBuffer instance');
assert(abResult.byteLength === 2, 'arrayBuffer length');
const view = new Uint8Array(abResult);
assert(view[0] === 72 && view[1] === 105, 'arrayBuffer bytes');

// .slice()
const tail = b1.slice(7);
let tailText;
tail.text().then(t => { tailText = t; });
flush();
advanceTime(10);
flush();
assert(tailText === 'world', 'slice text: ' + tailText);
assert(tail.size === 5, 'slice size: ' + tail.size);

// slice with contentType
const t2 = b1.slice(0, 5, 'application/octet-stream');
assert(t2.type === 'application/octet-stream', 'slice contentType: ' + t2.type);
