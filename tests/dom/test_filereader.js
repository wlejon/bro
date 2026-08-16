// FileReader, and dropped files that are real File objects.
//
// bro had Blob and File (brokit's) but no FileReader at all, and a drop handed
// the page `{name, path}` stubs — no size, no type, no bytes. Between them that
// makes the whole drag-and-drop import path, which is how most apps take a file
// in, impossible to write: three.js's editor reads every model it imports with
// `new FileReader()` and resolves the model's embedded textures through
// `URL.createObjectURL(file)`, and neither had anything to work with.

const fs = require('fs');
const path = require('path');

assert(typeof FileReader === 'function', 'FileReader exists');
assert(FileReader.EMPTY === 0 && FileReader.LOADING === 1 && FileReader.DONE === 2,
       'the readyState constants are on the constructor');

// ------------------------------------------------------------ reading a Blob
const bytes = new Uint8Array([0x62, 0x72, 0x6f, 0x00, 0xff, 0x41]);   // "bro\0\xffA"

function read(method, blob, arg) {
    return new Promise(function (resolve, reject) {
        const r = new FileReader();
        const seen = [];
        r.onloadstart = function () { seen.push('loadstart'); };
        r.onprogress = function () { seen.push('progress'); };
        r.onload = function () { seen.push('load'); };
        r.onloadend = function () {
            seen.push('loadend');
            resolve({ result: r.result, readyState: r.readyState, events: seen });
        };
        r.onerror = function () { reject(r.error || new Error('read failed')); };
        r[method](blob, arg);
        // Assigning the handlers after the call must still work: a read never
        // completes synchronously.
        assert(r.readyState === FileReader.LOADING, method + ' enters LOADING');
    });
}

const textBlob = new Blob(['hello bro'], { type: 'text/plain' });
const binBlob = new Blob([bytes], { type: 'application/octet-stream' });

await (async function () {
    const t = await read('readAsText', textBlob);
    assert(t.result === 'hello bro', 'readAsText gives the text, got ' + JSON.stringify(t.result));
    assert(t.readyState === FileReader.DONE, 'readyState is DONE at loadend');
    assert(t.events.join(',') === 'loadstart,progress,load,loadend',
           'the event sequence is loadstart/progress/load/loadend, got ' + t.events.join(','));

    const b = await read('readAsArrayBuffer', binBlob);
    assert(b.result instanceof ArrayBuffer, 'readAsArrayBuffer gives an ArrayBuffer');
    const got = new Uint8Array(b.result);
    assert(got.length === bytes.length, 'all bytes came through, got ' + got.length);
    for (let i = 0; i < bytes.length; i++)
        assert(got[i] === bytes[i], 'byte ' + i + ' is ' + bytes[i] + ', got ' + got[i]);

    const d = await read('readAsDataURL', binBlob);
    assert(d.result === 'data:application/octet-stream;base64,YnJvAP9B',
           'readAsDataURL base64-encodes with the blob type, got ' + d.result);

    const s = await read('readAsBinaryString', binBlob);
    assert(s.result.length === bytes.length, 'readAsBinaryString keeps every byte');
    assert(s.result.charCodeAt(4) === 0xff, 'high bytes survive as their own code unit');

    // The listener form, not just the on* property.
    const viaListener = await new Promise(function (resolve) {
        const r = new FileReader();
        r.addEventListener('load', function (e) { resolve(e.target.result); });
        r.readAsText(new Blob(['listener']));
    });
    assert(viaListener === 'listener',
           'addEventListener("load") fires and event.target.result is set');

    // abort() stops the read and reports it.
    const aborted = await new Promise(function (resolve) {
        const r = new FileReader();
        const evs = [];
        r.onabort = function () { evs.push('abort'); };
        r.onload = function () { evs.push('load'); };
        r.onloadend = function () { resolve({ evs: evs, result: r.result }); };
        r.readAsText(new Blob(['x'.repeat(1000)]));
        r.abort();
    });
    assert(aborted.evs.join(',') === 'abort', 'abort fires abort, not load');
    assert(aborted.result === null, 'an aborted read leaves result null');

    // ------------------------------------------------- a dropped file is a File
    const tmp = path.join(bro.appDir, 'filereader_drop_test.json');
    const payload = '{"metadata":{"type":"App"},"n":42}';
    fs.writeFileSync(tmp, payload);

    const root = document.getElementById('root');
    root.innerHTML = '<div id="dropzone" style="position:absolute;left:0;top:0;' +
                     'width:300px;height:200px"></div>';
    flush();

    const dropped = await new Promise(function (resolve) {
        document.getElementById('dropzone').addEventListener('drop', function (e) {
            e.preventDefault();
            resolve(e.dataTransfer);
        });
        dropFiles(100, 100, [tmp]);
    });

    assert(Array.isArray(dropped.types) || dropped.types.length >= 0,
           'dataTransfer.types exists');
    assert(dropped.types[0] === 'Files',
           'types[0] is "Files" for a file drop, got ' + JSON.stringify(dropped.types));
    assert(dropped.files.length === 1, 'one file arrived');

    const f = dropped.files[0];
    assert(f instanceof File, 'the dropped entry is a real File');
    assert(f.name === 'filereader_drop_test.json',
           'name is the basename, got ' + f.name);
    assert(f.size === payload.length, 'size is the byte count, got ' + f.size);
    assert(f.type === 'application/json',
           'type comes from the extension, got ' + JSON.stringify(f.type));
    assert(f.path === tmp, 'the real filesystem path is still there');

    // The three things an importer does with it.
    const viaReader = await read('readAsText', f);
    assert(viaReader.result === payload, 'FileReader reads a dropped file');
    assert(JSON.parse(viaReader.result).n === 42, 'and the bytes parse');

    const url = URL.createObjectURL(f);
    assert(typeof url === 'string' && url.indexOf('blob:') === 0,
           'createObjectURL(file) gives a blob URL, got ' + url);
    const fetched = await fetch(url);
    assert((await fetched.text()) === payload,
           'the blob URL serves the file\'s real bytes — an empty one is how ' +
           'embedded textures silently fail to load');
    URL.revokeObjectURL(url);

    assert((await f.text()) === payload, 'blob.text() works on it too');

    fs.unlinkSync(tmp);
    root.innerHTML = '';
    console.log('PASS: FileReader and dropped File objects');
})();
