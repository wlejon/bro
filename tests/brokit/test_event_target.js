// Test standalone EventTarget.

assert(typeof EventTarget === 'function', 'EventTarget exists');
const et = new EventTarget();

let count = 0;
let lastEvent = null;
const handler = (e) => { count++; lastEvent = e; };
et.addEventListener('ping', handler);

const ev = new Event('ping');
const r = et.dispatchEvent(ev);
assert(r === true || r === undefined, 'dispatchEvent return: ' + r);
assert(count === 1, 'fired once: ' + count);
assert(lastEvent && lastEvent.type === 'ping', 'event.type: ' + (lastEvent && lastEvent.type));

// removeEventListener
et.removeEventListener('ping', handler);
et.dispatchEvent(new Event('ping'));
assert(count === 1, 'no fire after remove: ' + count);

// once option
let onceCount = 0;
et.addEventListener('once', () => { onceCount++; }, { once: true });
et.dispatchEvent(new Event('once'));
et.dispatchEvent(new Event('once'));
assert(onceCount === 1, 'once fires once: ' + onceCount);

// CustomEvent with detail
if (typeof CustomEvent === 'function') {
    let detail = null;
    et.addEventListener('c', (e) => { detail = e.detail; });
    et.dispatchEvent(new CustomEvent('c', { detail: { a: 1 } }));
    assert(detail && detail.a === 1, 'CustomEvent detail: ' + JSON.stringify(detail));
} else {
    console.warn('CustomEvent missing'); // BUG: CustomEvent-missing
}

// Multiple listeners for same type
let total = 0;
et.addEventListener('m', () => total++);
et.addEventListener('m', () => total += 10);
et.dispatchEvent(new Event('m'));
assert(total === 11, 'multiple listeners: ' + total);
