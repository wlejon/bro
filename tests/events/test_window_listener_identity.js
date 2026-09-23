// A window listener's identity is (type, callback, capture), as on any
// EventTarget (DOM 2.7, "add an event listener" / "remove an event listener"):
//   * adding the same triple twice registers it once;
//   * the same callback with capture and without is two registrations;
//   * removeEventListener removes only the registration whose capture flag it
//     names, so a bubble-phase removal leaves a capture listener in place.

let n = 0;
const f = () => { n++; };

// Duplicate add is a no-op.
window.addEventListener('wli-dup', f);
window.addEventListener('wli-dup', f);
window.addEventListener('wli-dup', f, { capture: false });
window.dispatchEvent(new Event('wli-dup'));
assert(n === 1, 'duplicate adds register once, fired ' + n);
window.removeEventListener('wli-dup', f);
n = 0;
window.dispatchEvent(new Event('wli-dup'));
assert(n === 0, 'one remove clears the single registration, fired ' + n);

// Capture and bubble are distinct registrations.
n = 0;
window.addEventListener('wli-cap', f, true);
window.addEventListener('wli-cap', f, false);
window.dispatchEvent(new Event('wli-cap'));
assert(n === 2, 'capture + bubble are two registrations, fired ' + n);

// Removing the bubble one leaves the capture one.
window.removeEventListener('wli-cap', f);
n = 0;
window.dispatchEvent(new Event('wli-cap'));
assert(n === 1, 'bubble removal leaves the capture listener, fired ' + n);

// Removing with the wrong flag again is a no-op; the right flag removes it.
window.removeEventListener('wli-cap', f, false);
n = 0;
window.dispatchEvent(new Event('wli-cap'));
assert(n === 1, 'a second bubble removal changes nothing, fired ' + n);
window.removeEventListener('wli-cap', f, { capture: true });
n = 0;
window.dispatchEvent(new Event('wli-cap'));
assert(n === 0, 'capture removal clears it, fired ' + n);

// once + re-add: the once registration is gone after firing, so a re-add
// counts as new.
n = 0;
window.addEventListener('wli-once', f, { once: true });
window.dispatchEvent(new Event('wli-once'));
window.dispatchEvent(new Event('wli-once'));
assert(n === 1, 'once fires once, fired ' + n);
window.addEventListener('wli-once', f, { once: true });
window.dispatchEvent(new Event('wli-once'));
assert(n === 2, 'a re-add after once fired registers again, fired ' + n);

console.log('window listener identity OK');
