// Worker for test_compression_worker.js — exercises CompressionStream /
// DecompressionStream inside a worker realm (installAll runs per-realm, so
// the globals must exist here too).

async function readAll(stream) {
    const reader = stream.getReader();
    const chunks = [];
    let total = 0;
    for (;;) {
        const r = await reader.read();
        if (r.done) break;
        chunks.push(r.value);
        total += r.value.length;
    }
    const out = new Uint8Array(total);
    let off = 0;
    for (const c of chunks) { out.set(c, off); off += c.length; }
    return out;
}

function streamFrom(chunks) {
    return new ReadableStream({
        start(c) {
            for (const ch of chunks) c.enqueue(ch);
            c.close();
        }
    });
}

self.onmessage = async (e) => {
    const data = e.data;
    try {
        if (data.cmd === 'roundtrip') {
            const input = new Uint8Array(data.bytes);
            const compressed = await readAll(
                streamFrom([input]).pipeThrough(new CompressionStream(data.format)));
            const back = await readAll(
                streamFrom([compressed]).pipeThrough(new DecompressionStream(data.format)));
            let equal = back.length === input.length;
            for (let i = 0; equal && i < back.length; i++) {
                if (back[i] !== input[i]) equal = false;
            }
            self.postMessage({
                ok: true,
                hasClasses: typeof CompressionStream === 'function' &&
                            typeof DecompressionStream === 'function',
                compressedLength: compressed.length,
                equal
            });
        } else if (data.cmd === 'badformat') {
            let threw = null;
            try { new CompressionStream('nope'); } catch (err) { threw = err; }
            self.postMessage({ ok: true, isTypeError: threw instanceof TypeError });
        } else {
            self.postMessage({ ok: false, error: 'unknown cmd' });
        }
    } catch (err) {
        self.postMessage({ ok: false, error: String(err) });
    }
};
