// Character completeness: crouch/stance (setShape), inner rigid body
// (innerBody: true — sensors/raycasts/bodies can SEE the character), and
// character-vs-character collision (CharacterVsCharacterCollisionSimple —
// two characters stop instead of ghosting through each other).

Physics.destroyAll();
Physics.setGravity(0, -9.81, 0);
Physics.setTimeStep(1 / 60);

// Floor slab: top face at y = 0.
Physics.createBody({
    shape: 'box', halfExtents: { x: 50, y: 0.5, z: 50 },
    position: { x: 0, y: -0.5, z: 0 }, static: true,
});

// ===========================================================================
// Crouch / stance: setShape succeeds in the open, fails under a low ceiling.
// ===========================================================================
const STAND = { radius: 0.3, halfHeight: 0.6 };   // total height 1.8
const CROUCH = { radius: 0.3, halfHeight: 0.1 };  // total height 0.8

const ch = Physics.createCharacter({
    radius: STAND.radius, halfHeight: STAND.halfHeight,
    position: { x: 0, y: 1.0, z: 0 },
});
assert(typeof ch.setShape === 'function', 'character has setShape');
advanceTime(500);
assert(ch.getState().isGrounded, 'standing character grounded');

// Crouch in the open: always room to shrink.
let ok = ch.setShape({ shape: 'capsule', radius: CROUCH.radius, halfHeight: CROUCH.halfHeight });
assert(ok === true, 'crouching (shrinking) succeeds');
advanceTime(300);
// Capsule center settles at halfHeight + radius above the floor.
let y = ch.getState().position.y;
assert(Math.abs(y - (CROUCH.halfHeight + CROUCH.radius)) < 0.08,
       'crouched center height ~0.4, got ' + y);

// Stand back up in the open: fine.
ok = ch.setShape({ shape: 'capsule', radius: STAND.radius, halfHeight: STAND.halfHeight });
assert(ok === true, 'standing up in the open succeeds');
advanceTime(300);

// Now crouch under a low ceiling (clearance 1.0 — crouched fits, standing
// does not) and verify standing up is REFUSED while under it.
Physics.createBody({
    shape: 'box', halfExtents: { x: 2, y: 0.25, z: 2 },
    position: { x: 10, y: 1.25, z: 0 }, static: true,   // underside at y = 1.0
});
ok = ch.setShape({ shape: 'capsule', radius: CROUCH.radius, halfHeight: CROUCH.halfHeight });
assert(ok === true, 'crouched before walking under the ceiling');
ch.setPosition(10, CROUCH.halfHeight + CROUCH.radius + 0.02, 0);
advanceTime(300);
assert(ch.getState().isGrounded, 'crouched character grounded under the ceiling');

ok = ch.setShape({ shape: 'capsule', radius: STAND.radius, halfHeight: STAND.halfHeight });
assert(ok === false, 'standing up under a low ceiling is refused (no room)');
// Still crouched and functional.
advanceTime(200);
y = ch.getState().position.y;
assert(y < 0.6, 'character remained crouched, y=' + y);

// Walk out and stand up.
ch.setPosition(0, 0.42, 0);
advanceTime(300);
ok = ch.setShape({ shape: 'capsule', radius: STAND.radius, halfHeight: STAND.halfHeight });
assert(ok === true, 'standing up succeeds once clear of the ceiling');
ch.destroy();

// ===========================================================================
// Inner body: a sensor volume reports the character entering and leaving
// (impossible without innerBody — CharacterVirtual never enters the
// broadphase).
// ===========================================================================
const gate = Physics.createBody({
    shape: 'box', halfExtents: { x: 1, y: 2, z: 3 },
    position: { x: 5, y: 1, z: 0 },
    static: true, sensor: true,
});

const walker = Physics.createCharacter({
    radius: 0.3, halfHeight: 0.6,
    position: { x: 0, y: 1.0, z: 0 },
    innerBody: true,
});
assert(typeof walker.innerBody === 'number' && walker.innerBody > 0,
       'innerBody tag exposed, got ' + walker.innerBody);
// The inner body is visible to queries: a ray through the character hits it.
advanceTime(200);
{
    const hits = Physics.raycast(-2, 0.9, 0, 1, 0, 0, 10);
    assert(hits.some(h => h.bodyId === walker.innerBody),
           'raycast sees the character via its inner body');
}
// The inner body must refuse a direct destroy (owned by the character).
Physics.destroyBody(walker.innerBody);
{
    const hits = Physics.raycast(-2, 0.9, 0, 1, 0, 0, 10);
    assert(hits.some(h => h.bodyId === walker.innerBody),
           'inner body survives a direct destroyBody (character owns it)');
}

walker.setVelocity(2, 0, 0);   // walk through the sensor gate at x=5
let enter = null, leave = null;
for (let i = 0; i < 500 && leave === null; i++) {
    advanceTime(16);
    for (const e of Physics.getContacts()) {
        const pair = (e.body1 === gate && e.body2 === walker.innerBody) ||
                     (e.body1 === walker.innerBody && e.body2 === gate);
        if (!pair) continue;
        if (e.type === 'added' && enter === null) enter = e;
        if (e.type === 'removed' && enter !== null) leave = e;
    }
}
assert(enter !== null, 'sensor reported the character entering');
assert(enter.sensor === true, 'enter event flagged sensor:true');
assert(leave !== null, 'sensor reported the character leaving');
walker.setVelocity(0, 0, 0);
walker.destroy();

// ===========================================================================
// Character-vs-character: two characters walked into each other stop and do
// not interpenetrate (they used to ghost straight through).
// ===========================================================================
const R = 0.3;
const a = Physics.createCharacter({
    radius: R, halfHeight: 0.6, position: { x: -2, y: 1.0, z: 0 },
});
const b = Physics.createCharacter({
    radius: R, halfHeight: 0.6, position: { x: 2, y: 1.0, z: 0 },
});
advanceTime(300);
a.setVelocity(2, 0, 0);
b.setVelocity(-2, 0, 0);
advanceTime(3000);   // unobstructed they would each travel 6 m and swap sides

const ax = a.getState().position.x;
const bx = b.getState().position.x;
assert(ax < bx, 'characters did not pass through each other (a.x=' + ax + ' b.x=' + bx + ')');
// Capsule centers can't overlap closer than 2*radius (small padding slack).
assert(bx - ax > 2 * R - 0.1,
       'characters not interpenetrating, gap=' + (bx - ax));
// And they actually met near the middle (collision stopped them, not slow walk).
assert(Math.abs(ax) < 1.0 && Math.abs(bx) < 1.0,
       'characters met near the center (' + ax + ', ' + bx + ')');

a.setVelocity(0, 0, 0);
b.setVelocity(0, 0, 0);
a.destroy();
b.destroy();
Physics.destroyAll();
