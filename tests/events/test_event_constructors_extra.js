// HashChangeEvent, PopStateEvent and FormDataEvent: constructible, Events,
// carrying their init-dictionary members with the spec's defaults, and
// dispatchable to a listener that sees the very object.

for (const name of ['HashChangeEvent', 'PopStateEvent', 'FormDataEvent']) {
    assert(typeof globalThis[name] === 'function', name + ' exists');
    assert(window[name] === globalThis[name], name + ' is on window');
}

const h = new HashChangeEvent('hashchange', { oldURL: 'bro://app/#a', newURL: 'bro://app/#b' });
assert(h instanceof Event, 'HashChangeEvent is an Event');
assert(h.type === 'hashchange', 'type');
assert(h.oldURL === 'bro://app/#a' && h.newURL === 'bro://app/#b', 'oldURL / newURL');
const hd = new HashChangeEvent('hashchange');
assert(hd.oldURL === '' && hd.newURL === '', 'URLs default to empty');
assert(hd.bubbles === false && hd.cancelable === false, 'bubbles / cancelable default false');

const state = { page: 3 };
const ps = new PopStateEvent('popstate', { state });
assert(ps instanceof Event, 'PopStateEvent is an Event');
assert(ps.state === state, 'state is the given object');
assert(new PopStateEvent('popstate').state === null, 'state defaults to null');
assert(ps.hasUAVisualTransition === false, 'hasUAVisualTransition defaults to false');

const fd = new FormData();
fd.append('k', 'v');
const fe = new FormDataEvent('formdata', { formData: fd, bubbles: true });
assert(fe instanceof Event, 'FormDataEvent is an Event');
assert(fe.formData === fd, 'formData is the given FormData');
assert(fe.bubbles === true, 'bubbles from the init dictionary');

// Dispatch: window listeners for the navigation events, an element for formdata.
let seenHash = null;
let seenPop = null;
window.addEventListener('hashchange', (e) => { seenHash = e; });
window.addEventListener('popstate', (e) => { seenPop = e; });
window.dispatchEvent(h);
window.dispatchEvent(ps);
assert(seenHash === h, 'hashchange listener got the dispatched object');
assert(seenHash.newURL === 'bro://app/#b', 'and its members');
assert(seenPop === ps && seenPop.state === state, 'popstate listener got state');

const form = document.createElement('form');
document.body.appendChild(form);
let seenForm = null;
form.addEventListener('formdata', (e) => { seenForm = e; });
form.dispatchEvent(fe);
assert(seenForm === fe && seenForm.formData.get('k') === 'v', 'formdata listener got formData');

console.log('extra event constructors: OK');
