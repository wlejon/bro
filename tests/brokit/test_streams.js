// Test ReadableStream, WritableStream, TransformStream.

assert(typeof ReadableStream === 'function', 'ReadableStream exists');
assert(typeof WritableStream === 'function', 'WritableStream exists');

// ReadableStream from a start function
const rs = new ReadableStream({
    start(controller) {
        controller.enqueue('a');
        controller.enqueue('b');
        controller.enqueue('c');
        controller.close();
    }
});
const reader = rs.getReader();
const chunks = [];
function pump() {
    return reader.read().then(({ value, done }) => {
        if (done) return;
        chunks.push(value);
        return pump();
    });
}
let pumpDone = false;
pump().then(() => { pumpDone = true; });
for (let i = 0; i < 10 && !pumpDone; i++) { flush(); advanceTime(10); flush(); }
assert(pumpDone, 'pump completed');
assert(chunks.length === 3, 'got 3 chunks: ' + chunks.length);
assert(chunks[0] === 'a' && chunks[2] === 'c', 'chunks values');

// WritableStream
const writes = [];
const ws = new WritableStream({
    write(chunk) { writes.push(chunk); },
    close() {}
});
const writer = ws.getWriter();
let closeDone = false;
writer.write('x').then(() => writer.write('y')).then(() => writer.close()).then(() => { closeDone = true; });
for (let i = 0; i < 10 && !closeDone; i++) { flush(); advanceTime(10); flush(); }
assert(closeDone, 'writer close completed');
assert(writes.length === 2, 'writes: ' + writes.length);
assert(writes[0] === 'x', 'first write');

// TransformStream
if (typeof TransformStream === 'function') {
    const ts = new TransformStream({
        transform(chunk, controller) {
            controller.enqueue(chunk.toUpperCase());
        }
    });
    const rs2 = new ReadableStream({
        start(c) { c.enqueue('hi'); c.close(); }
    });
    let outChunks = [];
    let pipeDone = false;
    rs2.pipeThrough(ts);
    const r2 = ts.readable.getReader();
    function pump2() {
        return r2.read().then(({ value, done }) => {
            if (done) { pipeDone = true; return; }
            outChunks.push(value);
            return pump2();
        });
    }
    pump2();
    for (let i = 0; i < 20 && !pipeDone; i++) { flush(); advanceTime(10); flush(); }
    assert(pipeDone, 'transform pipe done');
    assert(outChunks[0] === 'HI', 'transform output: ' + outChunks[0]);
} else {
    console.warn('TransformStream not implemented'); // BUG: TransformStream-missing
}
