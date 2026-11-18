// The page's own absolute URLs fetch: `bro://app/<path>` (what
// `new URL(x, location.href)` makes) reads the app folder's file, the path
// percent-decoded and the query and fragment dropped, in the page and in a
// blob worker. pixi v8's worker fetches its textures by absolute URL, and
// every one was a 404.

const url = new URL('index.html', location.href).href;
assert(url.startsWith('bro://app/'), 'the page resolves to bro://app/, got ' + url);
const local = await (await fetch('index.html')).text();

const res = await fetch(url);
assert(res.status === 200, 'an absolute app URL fetches, got ' + res.status);
assert((await res.text()) === local, 'it is the app folder\'s file');

const decorated = await fetch(url.replace('index.html', 'index%2Ehtml') + '?v=1#top');
assert(decorated.status === 200, 'percent-decoded, query and fragment dropped, got ' + decorated.status);

const missing = await fetch(new URL('no-such-file.txt', location.href).href);
assert(missing.status === 404, 'a missing app file is a 404, got ' + missing.status);

const src = 'self.onmessage = async e => { const r = await fetch(e.data); postMessage(r.status + " " + (await r.text()).length); };';
const worker = new Worker(URL.createObjectURL(new Blob([src], { type: 'text/javascript' })));
const fromWorker = await new Promise((resolve) => {
    worker.onmessage = (e) => resolve(e.data);
    worker.postMessage(url);
});
worker.terminate();
assert(fromWorker === '200 ' + local.length, 'a blob worker fetches the app URL too, got ' + fromWorker);
