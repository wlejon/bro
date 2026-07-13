// Sensor (trigger) enter AND leave.
//
// A sensor exit used to be reported with sensor:false, because Jolt's
// OnContactRemoved hands the engine only body IDs — so an app could see a
// trigger entered but never cleanly see it left, which is half of what a
// trigger is for. This drops a dynamic sphere straight through a static sensor
// slab and requires both edges to arrive correctly labelled.

Physics.destroyAll();
Physics.setGravity(0, -9.81, 0);
Physics.setTimeStep(1 / 60);

const sensor = Physics.createBody({
    shape: 'box',
    halfExtents: { x: 3, y: 0.5, z: 3 },
    position: { x: 0, y: 0, z: 0 },
    static: true,
    sensor: true,
});
assert(typeof sensor === 'number' && sensor > 0, 'sensor body created');

const ball = Physics.createBody({
    shape: 'sphere',
    radius: 0.5,
    position: { x: 0, y: 6, z: 0 },
});
assert(typeof ball === 'number' && ball > 0, 'falling body created');

// Fall through the slab, draining events every step so none are missed
// (getContacts drains — whatever it returns is gone from the queue).
let entered = null;
let exited = null;
for (let i = 0; i < 400 && exited === null; i++) {
    advanceTime(16);
    for (const e of Physics.getContacts()) {
        const involvesPair =
            (e.body1 === sensor && e.body2 === ball) ||
            (e.body1 === ball && e.body2 === sensor);
        if (!involvesPair) continue;
        if (e.type === 'added' && entered === null) entered = e;
        if (e.type === 'removed' && entered !== null) exited = e;
    }
}

assert(entered !== null, 'sensor reported the ball entering');
assert(entered.sensor === true, 'enter event is flagged sensor:true');

assert(exited !== null, 'sensor reported the ball leaving');
assert(exited.sensor === true,
       'leave event is flagged sensor:true (was false — the bug)');

// The ball passes through: a sensor generates no collision response.
const pos = Physics.getTransform(ball).position;
assert(pos.y < -1, 'ball fell through the sensor rather than landing on it, y=' + pos.y);

Physics.destroyAll();
