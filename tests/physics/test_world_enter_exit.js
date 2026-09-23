// PhysicsWorldHandle.enter() / exit(): push a sandbox world as the active one
// so plain `Physics.*` calls between them act on it, then pop back.

const w = Physics.createWorldHandle({ maxBodies: 64, gravity: { x: 0, y: -2.5, z: 0 } });
assert(typeof w.enter === 'function', 'PhysicsWorldHandle.enter exists');
assert(typeof w.exit === 'function', 'PhysicsWorldHandle.exit exists');

const defaultG = Physics.getGravity().y;
assert(Math.abs(defaultG + 2.5) > 1e-3, 'default world is not the sandbox, g=' + defaultG);

w.enter();
assert(Math.abs(Physics.getGravity().y + 2.5) < 1e-4,
       'inside enter(), Physics.getGravity reads the sandbox: ' + Physics.getGravity().y);
const tag = Physics.createBody({ shape: 'sphere', radius: 0.5, position: { x: 3, y: 7, z: 0 } });
Physics.setGravity(0, -1, 0);
// Forwarded handle calls nest inside an explicit enter().
assert(Math.abs(w.getGravity().y + 1) < 1e-4, 'forwarded call inside enter() sees the same world');
const p = Physics.getTransform(tag).position;
assert(Math.abs(p.y - 7) < 1e-3, 'the body made inside enter() is readable there: y=' + p.y);
w.exit();

assert(Math.abs(Physics.getGravity().y - defaultG) < 1e-4,
       'after exit(), the default world is active again: ' + Physics.getGravity().y);
// The body lives in the sandbox: read through the handle.
const q = w.getTransform(tag).position;
assert(Math.abs(q.x - 3) < 1e-3 && Math.abs(q.y - 7) < 1e-3,
       'the body is in the sandbox world: ' + q.x + ',' + q.y);
assert(Math.abs(w.getGravity().y + 1) < 1e-4, 'setGravity inside enter() hit the sandbox');

// Nested enter across two worlds pops in order.
const w2 = Physics.createWorldHandle({ maxBodies: 16, gravity: { x: 0, y: -7, z: 0 } });
w.enter();
w2.enter();
assert(Math.abs(Physics.getGravity().y + 7) < 1e-4, 'innermost enter() wins');
w2.exit();
assert(Math.abs(Physics.getGravity().y + 1) < 1e-4, 'exit() pops back to the outer world');
w.exit();
assert(Math.abs(Physics.getGravity().y - defaultG) < 1e-4, 'fully popped to default');

w2.destroy();
w.destroy();
console.log('PhysicsWorldHandle enter/exit OK');
