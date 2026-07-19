// Worker side of test_postmessage_typedarray.
//
// Report what each view actually deserialized as, then echo it back so the
// main thread can check the return leg too. Both legs run the same serializer,
// but only a round trip proves the tag survives rather than merely being
// written.

self.onmessage = (e) => {
    const seen = {};
    for (const key of Object.keys(e.data)) {
        const view = e.data[key];
        seen[key] = {
            ctor: view.constructor.name,
            bpe: view.BYTES_PER_ELEMENT,
            // Stringify: BigInt elements are not JSON/structured-clone-safe as
            // Numbers, and comparing as text keeps every type in one path.
            vals: Array.from(view, (v) => String(v)),
        };
    }
    self.postMessage({ seen, echoed: e.data });
};
