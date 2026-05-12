// Capabilities: register custom, ids assigned, callbacks fire.

const G = bro.ai.game;

let gateCount = 0, startCount = 0, advanceCount = 0;
const id = G.registerCapability("test_cap", {
    gate()    { gateCount++; return true; },
    start()   { startCount++; },
    advance() { advanceCount++; return true; },
});
assert(typeof id === 'number', 'registerCapability returns numeric id, got ' + typeof id);
assert(id >= 100, 'auto-allocated id >= 100, got ' + id);

// Re-register a different one to verify ids differ.
const id2 = G.registerCapability("test_cap_2", {
    gate() { return false; },
    advance() { return false; },
});
assert(id2 !== id, 'distinct capabilities get distinct ids, ' + id + ' vs ' + id2);

// Explicit id override.
const id3 = G.registerCapability("test_cap_explicit", { id: 250, advance(){return true;} });
assert(id3 === 250, 'explicit id honored, got ' + id3);

// The actual think/cap firing path goes via scene.attachAIWorld + node.attachAgent
// which requires a scene context. We just test the registration API surface here.

console.log('test_capabilities: OK');
