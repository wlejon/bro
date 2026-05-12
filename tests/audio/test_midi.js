// MIDI input — port enumeration and callback registration only, no hardware.

const ctx = new AudioContext();
const midi = ctx.createMidiInput();
assert(midi, 'createMidiInput returns object');
assert(typeof midi.availablePorts === 'function', 'availablePorts is a function');

const ports = midi.availablePorts();
console.log('ports:', JSON.stringify(ports));
assert(Array.isArray(ports), 'availablePorts returns array, got ' + (typeof ports));

assert(typeof midi.isOpen === 'boolean', 'isOpen is boolean');
assert(midi.isOpen === false, 'midi not open at start, got ' + midi.isOpen);

// Callback registration should not throw
let threw = false;
try {
    midi.onControlChange(1, (ch, cc, v) => {});
    midi.onPitchBend((ch, v) => {});
    midi.onRawEvent((ev) => {});
} catch (e) { threw = true; console.log('midi callback reg threw:', e.message); }
assert(!threw, 'onControlChange/onPitchBend/onRawEvent register without throwing');

// processEvents may be a no-op but must not throw
let t2 = false;
try { midi.processEvents(); } catch (e) { t2 = true; console.log('processEvents threw:', e.message); }
assert(!t2, 'processEvents does not throw');

// connectToAllocator with a valid allocator should not throw
{
    const alloc = ctx.createVoiceAllocator(4);
    let t3 = false;
    try { midi.connectToAllocator(alloc); } catch (e) { t3 = true; console.log('connectToAllocator threw:', e.message); }
    assert(!t3, 'connectToAllocator(allocator) does not throw');
}

// close() does not throw even when not open
let t4 = false;
try { midi.close(); } catch (e) { t4 = true; console.log('close threw:', e.message); }
assert(!t4, 'close() on unopened midi does not throw');
