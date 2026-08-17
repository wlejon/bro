// The physics probe: Jolt Physics integration in bronze_host.
//
// Tests:
// - Physics, PhysicsCharacter, PhysicsSoftBody globals
// - Rigid body creation (box, sphere, static ground)
// - Transforms, UserData, setPosition, setRotation
// - Linear velocity, angular velocity, impulses, forces, mass, damping
// - Gravity get/set, layer configuration
// - Kinematic motion
// - Compound shapes, Convex hulls
// - Raycasting (raycastClosest, raycast)
// - Overlaps (overlapSphere, overlapBox, overlapPoint)
// - Character controller (createCharacter, setVelocity, getState, destroy)
// - Soft body / cloth (createSoftBody, vertexCount, topology, vertices, pin, destroy)
// - Contact listener registration
// - getAllTransforms bulk buffer
// - Body destruction & cleanup

function say(label, value) {
    console.log('APP ' + label + '=' + value);
}

// ---------------------------------------------------------------------------
// 1. Global existence
// ---------------------------------------------------------------------------

say('global.hasPhysics', typeof Physics === 'object');
say('global.hasPhysicsCharacter', typeof PhysicsCharacter === 'function');
say('global.hasPhysicsSoftBody', typeof PhysicsSoftBody === 'function');

// ---------------------------------------------------------------------------
// 2. Rigid body creation
// ---------------------------------------------------------------------------

const groundTag = Physics.createBody({
    shape: 'box',
    halfExtents: { x: 50, y: 1, z: 50 },
    position: { x: 0, y: -1, z: 0 },
    isStatic: true,
    friction: 0.8,
    restitution: 0.1
});
say('body.groundTagValid', groundTag > 0);

const boxTag = Physics.createBody({
    shape: 'box',
    halfExtents: { x: 1, y: 1, z: 1 },
    position: { x: 0, y: 10, z: 0 },
    mass: 5.0,
    friction: 0.5,
    restitution: 0.3,
    userData: 42
});
say('body.boxTagValid', boxTag > 0 && boxTag !== groundTag);

const sphereTag = Physics.createBody({
    shape: 'sphere',
    radius: 0.5,
    position: { x: 2, y: 5, z: 0 },
    mass: 2.0
});
say('body.sphereTagValid', sphereTag > 0 && sphereTag !== boxTag);

// ---------------------------------------------------------------------------
// 3. Transforms & UserData
// ---------------------------------------------------------------------------

const tf = Physics.getTransform(boxTag);
say('box.pos_y', Math.round(tf.position.y));
say('box.userData', tf.userData);
say('box.getUserData', Physics.getUserData(boxTag));

Physics.setPosition(boxTag, 0, 15, 0);
const tf2 = Physics.getTransform(boxTag);
say('box.new_pos_y', Math.round(tf2.position.y));

Physics.setRotation(boxTag, 0, 0, 0, 1);
const tf3 = Physics.getTransform(boxTag);
say('box.rot_w', Math.round(tf3.rotation.w));

// ---------------------------------------------------------------------------
// 4. Velocity, Impulses & Mass
// ---------------------------------------------------------------------------

Physics.setLinearVelocity(boxTag, 1, 2, 3);
const vel = Physics.getVelocity(boxTag);
say('box.vel_x', Math.round(vel.linear.x));
say('box.vel_y', Math.round(vel.linear.y));
say('box.vel_z', Math.round(vel.linear.z));

Physics.addImpulse(boxTag, 0, 5, 0);
Physics.addForce(boxTag, 0, 10, 0);
Physics.addTorque(boxTag, 1, 0, 0);

Physics.setMass(boxTag, 10);
say('box.mass', Physics.getMass(boxTag));
const props = Physics.getBodyProperties(boxTag);
say('box.props.mass', props.mass);

// ---------------------------------------------------------------------------
// 5. Gravity & Layers
// ---------------------------------------------------------------------------

const origG = Physics.getGravity();
say('gravity.y', Math.round(origG.y));
Physics.setGravity(0, -20, 0);
const newG = Physics.getGravity();
say('gravity.new_y', Math.round(newG.y));
Physics.setGravity(0, -9.81, 0);

const layerOk = Physics.setLayers({
    names: ['default', 'player', 'enemy'],
    matrix: [
        true, true, true,
        true, true, false,
        true, false, true
    ]
});
say('layers.configured', layerOk);

// ---------------------------------------------------------------------------
// 6. Kinematic motion & Compound / Hull shapes
// ---------------------------------------------------------------------------

const kinTag = Physics.createBody({
    shape: 'box',
    halfExtents: { x: 1, y: 1, z: 1 },
    position: { x: 0, y: 0, z: 0 }
});
Physics.setKinematic(kinTag);
Physics.moveKinematic(kinTag, 0, 5, 0, 0);
const kinTf = Physics.getTransform(kinTag);
say('kinematic.pos_y', Math.round(kinTf.position.y));

const compoundTag = Physics.createBody({
    shape: 'compound',
    parts: [
        { shape: 'box', halfExtents: { x: 0.5, y: 0.5, z: 0.5 }, localPosition: { x: 0, y: 1, z: 0 } },
        { shape: 'sphere', radius: 0.5, localPosition: { x: 0, y: -1, z: 0 } }
    ],
    position: { x: 10, y: 10, z: 10 }
});
say('compound.created', compoundTag > 0);

const hullTag = Physics.createBody({
    shape: 'convexHull',
    points: [
        -1, -1, -1,
         1, -1, -1,
         1,  1, -1,
        -1,  1, -1,
         0,  0,  1
    ],
    position: { x: -10, y: 5, z: 0 }
});
say('hull.created', hullTag > 0);

// ---------------------------------------------------------------------------
// 7. Raycasting
// ---------------------------------------------------------------------------

const hit = Physics.raycastClosest({ x: 0, y: 20, z: 0 }, { x: 0, y: 0, z: 0 });
say('raycast.hitValid', hit !== null);
say('raycast.hitBody', hit ? hit.body === boxTag : false);
say('raycast.fractionValid', hit ? (hit.fraction > 0 && hit.fraction < 1) : false);

const hits = Physics.raycast({ x: 0, y: 20, z: 0 }, { x: 0, y: -5, z: 0 });
say('raycast.allHitsCount', hits.length >= 2);

// ---------------------------------------------------------------------------
// 8. Overlaps
// ---------------------------------------------------------------------------

const sphereHits = Physics.overlapSphere({ x: 0, y: 15, z: 0 }, 2);
say('overlap.sphereHitsBox', sphereHits.includes(boxTag));

const boxHits = Physics.overlapBox({ x: 0, y: 15, z: 0 }, { x: 2, y: 2, z: 2 });
say('overlap.boxHitsBox', boxHits.includes(boxTag));

const ptHits = Physics.overlapPoint(0, 15, 0);
say('overlap.ptHitsBox', ptHits.includes(boxTag));

// ---------------------------------------------------------------------------
// 9. Character Controller
// ---------------------------------------------------------------------------

const char = Physics.createCharacter({
    position: { x: 5, y: 2, z: 5 },
    radius: 0.4,
    halfHeight: 0.9,
    mass: 70
});
say('char.created', char !== null && typeof char === 'object');
char.setVelocity(0, 0, 5);
const cvel = char.getVelocity();
say('char.vel_z', Math.round(cvel.z));
const cpos = char.getPosition();
say('char.pos_x', Math.round(cpos.x));
const cstate = char.getState();
say('char.hasState', typeof cstate.groundState === 'string');
char.destroy();
say('char.destroyed', true);

// ---------------------------------------------------------------------------
// 10. Soft Body / Cloth
// ---------------------------------------------------------------------------

const cloth = Physics.createSoftBody({
    cloth: {
        gridX: 4,
        gridZ: 4,
        spacing: 0.5,
        mass: 1.0,
        pinned: 'corners'
    },
    position: { x: 0, y: 5, z: 0 }
});
say('cloth.created', cloth !== null && typeof cloth === 'object');
say('cloth.vertexCount', cloth.vertexCount);
const topo = cloth.topology();
say('cloth.topoValid', topo !== null && topo.gridX === 4 && topo.gridZ === 4);
const verts = cloth.vertices();
say('cloth.vertsValid', verts instanceof Float32Array && verts.length === 16 * 3);
cloth.pin(0, true);
cloth.setVertex(0, 0, 5, 0);
cloth.setVertexVelocity(0, 0, 0, 0);
say('cloth.pinned', true);
cloth.destroy();
say('cloth.destroyed', true);

// ---------------------------------------------------------------------------
// 11. Contact Listener Registration
// ---------------------------------------------------------------------------

Physics.onContact(function() {});
Physics.addEventListener('contact', function() {});
say('contact.registered', true);

// ---------------------------------------------------------------------------
// 12. Bulk Transforms Buffer
// ---------------------------------------------------------------------------

const allTf = Physics.getAllTransforms();
say('allTransforms.valid', allTf instanceof Float32Array && allTf.length > 0);

// ---------------------------------------------------------------------------
// 13. Cleanup
// ---------------------------------------------------------------------------

Physics.destroyBody(boxTag);
Physics.destroyBody(sphereTag);
Physics.destroyBody(groundTag);
Physics.destroyBody(kinTag);
Physics.destroyBody(compoundTag);
Physics.destroyBody(hullTag);
Physics.destroyAll();
say('cleanup.done', true);
