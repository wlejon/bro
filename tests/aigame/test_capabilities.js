// Capabilities: register custom, ids assigned, and the callbacks fire when a
// scene-node agent binding picks the capability through self.useCapability.

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

// Explicit id override, and its validation.
const id3 = G.registerCapability("test_cap_explicit", { id: 250, advance(){return true;} });
assert(id3 === 250, 'explicit id honored, got ' + id3);
let threw = false;
try { G.registerCapability("dup_id", { id: 250 }); } catch (e) { threw = e instanceof RangeError; }
assert(threw, 'an id another name holds is a RangeError');
threw = false;
try { G.registerCapability("bad_start", { start: 3 }); } catch (e) { threw = e instanceof TypeError; }
assert(threw, 'a non-function callback is a TypeError');

// ---- the binding drives a registered capability -----------------------------

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '64');
canvas.setAttribute('height', '64');
document.body.appendChild(canvas);
flush();
const scene = canvas.getContext('scene');
assert(scene !== null, 'scene context');

const world = G.createWorld();
scene.attachAIWorld(world, { stepHz: 60 });

// `this` is the spec; start gets useCapability's integers; advance gets
// (dt, elapsed) and ends the action when it says so.
const log = [];
const spec = {
    tag: 'kite-spec',
    gate() { log.push(['gate', this.tag]); return true; },
    start(a0, a1) { log.push(['start', this.tag, a0, a1]); },
    advance(dt, elapsed) {
        log.push(['advance', this.tag, dt, elapsed]);
        return elapsed >= 0.1;
    },
    cancel() { log.push(['cancel', this.tag]); },
};
G.registerCapability("kite", spec);

let thinks = 0;
const agent = G.createAgent({ x: 0, z: 0, speed: 2, radius: 0.4 });
world.addAgent(agent);
const node = scene.createNode('kiter');
node.attachAgent(world, agent, {
    capabilities: ["kite", "hold"],
    thinkHz: 60,
    think(self) {
        thinks++;
        if (thinks === 1) self.useCapability("kite", 7, 9);
        else self.hold(0.01);
    },
});

for (let i = 0; i < 20; i++) advanceTime(1000 / 60);

const starts = log.filter(e => e[0] === 'start');
const advances = log.filter(e => e[0] === 'advance');
assert(log.some(e => e[0] === 'gate'), 'gate ran before start');
assert(starts.length === 1, 'start ran once, got ' + starts.length);
assert(starts[0][1] === 'kite-spec', 'start ran with this = the spec');
assert(starts[0][2] === 7 && starts[0][3] === 9,
    'start got useCapability\'s args, got ' + starts[0][2] + ',' + starts[0][3]);
assert(advances.length >= 2, 'advance ran across frames, got ' + advances.length);
const last = advances[advances.length - 1];
assert(last[3] >= 0.1 && last[3] < 0.2, 'advance saw elapsed grow to its done point, got ' + last[3]);
assert(advances.every(e => e[2] > 0 && e[1] === 'kite-spec'), 'advance got a positive dt and this = spec');
assert(thinks > 1, 'the agent thought again once the capability reported done');

// A capability whose gate fails never starts.
let blockedStarts = 0;
G.registerCapability("blocked", { gate() { return false; }, start() { blockedStarts++; } });
const agent2 = G.createAgent({ x: 3, z: 0 });
world.addAgent(agent2);
const node2 = scene.createNode('blocked');
node2.attachAgent(world, agent2, {
    capabilities: ["blocked", "hold"], thinkHz: 60,
    think(self) { self.useCapability("blocked"); },
});
for (let i = 0; i < 5; i++) advanceTime(1000 / 60);
assert(blockedStarts === 0, 'a failing gate keeps start from running, got ' + blockedStarts);

// cancel() fires when the binding goes away mid-action.
G.registerCapability("long", { advance() { return false; }, cancel() { log.push(['cancel-long']); } });
const agent3 = G.createAgent({ x: -3, z: 0 });
world.addAgent(agent3);
const node3 = scene.createNode('long');
node3.attachAgent(world, agent3, {
    capabilities: ["long"], thinkHz: 60,
    think(self) { self.useCapability("long"); },
});
for (let i = 0; i < 3; i++) advanceTime(1000 / 60);
node3.detachAgent();
assert(log.some(e => e[0] === 'cancel-long'), 'cancel ran when the binding was detached mid-action');

// Unknown names are errors, not silent no-ops.
threw = false;
try {
    scene.createNode('x').attachAgent(world, G.createAgent({}), { capabilities: ["no_such_cap"] });
} catch (e) { threw = e instanceof TypeError; }
assert(threw, 'attachAgent with an unknown capability name throws a TypeError');
threw = false;
const node4 = scene.createNode('y');
node4.attachAgent(world, G.createAgent({}), {
    capabilities: ["hold"], thinkHz: 60,
    think(self) {
        try { self.useCapability("no_such_cap"); } catch (e) { threw = e instanceof TypeError; }
        self.hold(0.1);
    },
});
advanceTime(1000 / 60);
advanceTime(1000 / 60);
assert(threw, 'useCapability with an unregistered name throws a TypeError');

console.log('test_capabilities: OK');
