// Test CompressionStream / DecompressionStream (Compression Streams spec via
// brokit). Formats: gzip (RFC 1952), deflate (zlib, RFC 1950), deflate-raw
// (RFC 1951). Driven through real streams: pipeThrough + reading all chunks.

// ── Existence + shape ────────────────────────────────────────────────────
assert(typeof CompressionStream === 'function', 'CompressionStream exists');
assert(typeof DecompressionStream === 'function', 'DecompressionStream exists');
{
    const cs = new CompressionStream('gzip');
    assert(cs.readable instanceof ReadableStream, 'readable is a ReadableStream');
    assert(cs.writable instanceof WritableStream, 'writable is a WritableStream');
}

// ── Unknown format → TypeError ───────────────────────────────────────────
for (const bad of ['br', 'zstd', 'GZIP', '']) {
    let threw = null;
    try { new CompressionStream(bad); } catch (e) { threw = e; }
    assert(threw instanceof TypeError, 'CompressionStream("' + bad + '") throws TypeError');
    threw = null;
    try { new DecompressionStream(bad); } catch (e) { threw = e; }
    assert(threw instanceof TypeError, 'DecompressionStream("' + bad + '") throws TypeError');
}

// ── Helpers ──────────────────────────────────────────────────────────────
const enc = new TextEncoder();
const dec = new TextDecoder();

function streamFrom(chunks) {
    return new ReadableStream({
        start(c) {
            for (const ch of chunks) c.enqueue(ch);
            c.close();
        }
    });
}

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
    return { bytes: out, chunkCount: chunks.length };
}

// Writer-driven variant so error tests observe the write/close rejection
// directly (pipeThrough surfaces errors on the readable instead).
async function runThrough(ts, chunks) {
    const writer = ts.writable.getWriter();
    for (const c of chunks) await writer.write(c);
    await writer.close();
    return readAll(ts.readable);
}

function bytesEqual(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}

function b64ToBytes(b64) {
    const bin = atob(b64);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
}

function patternedBytes(n) {
    const out = new Uint8Array(n);
    let x = 123456789;
    for (let i = 0; i < n; i++) {
        x ^= x << 13; x ^= x >>> 17; x ^= x << 5; x |= 0;
        out[i] = x & 0xff;
    }
    return out;
}

// ── Round-trip via pipeThrough, all three formats ────────────────────────
const text = 'bro compression streams: the quick brown fox jumps over the lazy dog 🦊🐶 '.repeat(20);
const textBytes = enc.encode(text);

for (const format of ['gzip', 'deflate', 'deflate-raw']) {
    const compressed = await readAll(
        streamFrom([textBytes]).pipeThrough(new CompressionStream(format)));
    assert(compressed.bytes.length > 0, format + ': compression produced output');
    assert(compressed.bytes.length < textBytes.length,
           format + ': repetitive text actually compressed (' +
           compressed.bytes.length + ' < ' + textBytes.length + ')');

    const decompressed = await readAll(
        streamFrom([compressed.bytes]).pipeThrough(new DecompressionStream(format)));
    assert(bytesEqual(decompressed.bytes, textBytes), format + ': round-trip byte-equal');
    assert(dec.decode(decompressed.bytes) === text, format + ': round-trip text-equal');
}

// ── Full pipe chain: compress |> decompress in one pipeline ──────────────
{
    const out = await readAll(
        streamFrom([textBytes])
            .pipeThrough(new CompressionStream('gzip'))
            .pipeThrough(new DecompressionStream('gzip')));
    assert(bytesEqual(out.bytes, textBytes), 'chained pipeThrough round-trip');
}

// ── Incremental: 1 MB in 4 KB writes, streamed multi-chunk output ────────
{
    const big = patternedBytes(1024 * 1024); // xorshift bytes — incompressible
    const writes = [];
    for (let off = 0; off < big.length; off += 4096) {
        writes.push(big.subarray(off, off + 4096));
    }
    const compressed = await readAll(
        streamFrom(writes).pipeThrough(new CompressionStream('gzip')));
    assert(compressed.chunkCount > 1,
           'incremental: compressed output arrived in ' + compressed.chunkCount + ' chunks (>1)');
    const back = await readAll(
        streamFrom([compressed.bytes]).pipeThrough(new DecompressionStream('gzip')));
    assert(back.chunkCount > 1,
           'incremental: decompressed output arrived in ' + back.chunkCount + ' chunks (>1)');
    assert(bytesEqual(back.bytes, big), 'incremental: 1 MB round-trip byte-equal');
}

// ── Interop: decompress a known-good python gzip blob ────────────────────
// python3 -c "import gzip,base64; print(base64.b64encode(gzip.compress(TEXT, mtime=0)))"
const PY_TEXT = 'The quick brown fox jumps over the lazy dog. bro CompressionStream interop fixture 0123456789.';
const PY_GZIP_B64 = 'H4sIAAAAAAAC/xXKWxpDMBAG0K38K/BRl/JsCbWBYFRoMjFJSq2+PJ/TzYQt6mFFL7xbTHxgicZ58JcE4eKPOn8Y+Z3cBS0bJ+S9ZvsKQspA20DCDpM+QhRCmj3yoqyedZP8ATFF+ZZeAAAA';
{
    const out = await readAll(
        streamFrom([b64ToBytes(PY_GZIP_B64)]).pipeThrough(new DecompressionStream('gzip')));
    assert(dec.decode(out.bytes) === PY_TEXT, 'python gzip fixture decompresses to exact text');
}

// ── Compress-side interop: gzip container structure + round-trip ─────────
// (compressed bytes are not canonical across implementations, so verify the
// container: magic + method, and the ISIZE footer field)
{
    const g = await readAll(
        streamFrom([textBytes]).pipeThrough(new CompressionStream('gzip')));
    const b = g.bytes;
    assert(b[0] === 0x1f && b[1] === 0x8b, 'gzip output: magic bytes 1f 8b');
    assert(b[2] === 8, 'gzip output: CM = deflate');
    const isize = (b[b.length - 4] | (b[b.length - 3] << 8) |
                   (b[b.length - 2] << 16) | (b[b.length - 1] << 24)) >>> 0;
    assert(isize === textBytes.length,
           'gzip output: footer ISIZE = ' + isize + ' matches input length ' + textBytes.length);
    const rt = await readAll(
        streamFrom([b]).pipeThrough(new DecompressionStream('gzip')));
    assert(bytesEqual(rt.bytes, textBytes), 'JS-compressed gzip round-trips through DecompressionStream');
}

// ── Errors ───────────────────────────────────────────────────────────────
async function expectStreamError(promise, label) {
    let err = null;
    try { await promise; } catch (e) { err = e; }
    assert(err instanceof TypeError, label + ' errors with TypeError (got ' + err + ')');
}

// Non-BufferSource chunk
await expectStreamError(
    runThrough(new CompressionStream('gzip'), ['not a buffer']),
    'string chunk');
await expectStreamError(
    runThrough(new DecompressionStream('deflate'), [42]),
    'number chunk');

// Corrupt compressed data
{
    const corrupt = b64ToBytes(PY_GZIP_B64);
    corrupt[20] ^= 0xff; corrupt[21] ^= 0xff; corrupt[22] ^= 0xff;
    await expectStreamError(
        runThrough(new DecompressionStream('gzip'), [corrupt]),
        'corrupt gzip data');
}

// Truncated stream (close before deflate stream end)
{
    const whole = b64ToBytes(PY_GZIP_B64);
    await expectStreamError(
        runThrough(new DecompressionStream('gzip'), [whole.subarray(0, whole.length - 12)]),
        'truncated gzip stream');
}

// Trailing garbage after the compressed data
{
    const whole = b64ToBytes(PY_GZIP_B64);
    const junk = new Uint8Array(whole.length + 4);
    junk.set(whole, 0);
    junk.set([0xde, 0xad, 0xbe, 0xef], whole.length);
    await expectStreamError(
        runThrough(new DecompressionStream('gzip'), [junk]),
        'trailing garbage');
}

// Errored transform surfaces on the readable side of a pipeThrough too
{
    const corrupt = b64ToBytes(PY_GZIP_B64);
    corrupt[15] ^= 0xff; corrupt[16] ^= 0xff; corrupt[17] ^= 0xff;
    await expectStreamError(
        readAll(streamFrom([corrupt]).pipeThrough(new DecompressionStream('gzip'))),
        'pipeThrough of corrupt data');
}

// ── Teardown safety: abandon a half-written stream mid-flight ────────────
// (leak-prone path — Debug QuickJS asserts catch dangling codec state)
{
    const abandoned = new CompressionStream('gzip');
    const w = abandoned.writable.getWriter();
    await w.write(patternedBytes(64 * 1024));
    // Never closed, never aborted, readable never read — just dropped.
}
{
    const abandonedD = new DecompressionStream('deflate-raw');
    const w = abandonedD.writable.getWriter();
    const compressed = await readAll(
        streamFrom([textBytes]).pipeThrough(new CompressionStream('deflate-raw')));
    await w.write(compressed.bytes.subarray(0, 16));
    // Dropped mid-decompress.
}
