// new Worker(scriptURL) takes a URL as well as a path: the standard module
// form `new Worker(new URL('./w.js', import.meta.url), { type: 'module' })`
// hands it a URL object naming a file: URL. It used to throw "requires a
// script path" for anything but a string.

function waitFor(pred) {
    const deadline = Date.now() + 15000;
    while (!pred() && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
    return pred();
}

function roundTrip(w, label) {
    let got = null;
    w.onmessage = (e) => { got = e.data; };
    w.postMessage({ v: new Uint8Array([1, 2, 3]) });
    assert(waitFor(() => got !== null), label + ': worker replied');
    assert(got.vals.join() === '1,2,3', label + ': worker ran the script ' + JSON.stringify(got));
    w.terminate();
}

// A file: URL object, as new URL(rel, import.meta.url) produces.
const dirUrl = 'file:///' + bro.appDir.replace(/\\/g, '/').replace(/^\//, '') + '/';
const fileUrl = new URL('../workers/worker_transfer_view.js', dirUrl);
assert(fileUrl.protocol === 'file:', 'a file URL: ' + fileUrl.href);
roundTrip(new Worker(fileUrl, { type: 'module' }), 'URL object');

// The same URL as a string.
roundTrip(new Worker(fileUrl.href), 'file URL string');

// A path, as before.
roundTrip(new Worker('../workers/worker_transfer_view.js'), 'relative path');

// A scheme the loader cannot read is a TypeError at construction.
let threw = null;
try { new Worker('https://example.invalid/w.js'); } catch (e) { threw = e; }
assert(threw instanceof TypeError, 'an https URL throws a TypeError');

threw = null;
try { new Worker({}); } catch (e) { threw = e; }
assert(threw instanceof TypeError, 'a non-URL object throws a TypeError');

console.log('test_worker_url: OK');
