// Headless tests for the SceneObject hierarchy: Group + ComponentDefinition
// + ComponentInstance. Verifies:
//   - createGroup wraps members, preserves world transforms
//   - explodeGroup unwraps, preserves world transforms
//   - pickAt promotes to outermost-in-context (group), and innermost inside
//     an edit context
//   - makeComponent creates a shared definition + one instance
//   - inserting a second instance yields two positions for the same geometry
//   - serialize / deserialize round-trips the tree + components
//
// Run: bro-headless apps/scene-editor apps/scene-editor/test_groups.js

advanceTime(50);
flush();

const E = window.__editor;
const reg = E.registry;

function dist(a, b) {
    return Math.hypot(a[0]-b[0], a[1]-b[1], a[2]-b[2]);
}

// --- createGroup + transform preservation ----------------------------------

reg.clear();
reg._nextId = 1;
E.setupDefaultScene();
const box1 = reg.primitives[0];
const box2 = reg.create({ type: 'box', name: 'B2',
                          position: [5, 0, 0],
                          params: { sx: 1, sy: 1, sz: 1 } });

// Capture box2's world centroid before grouping.
const box2WorldBefore = box2.worldCentroid();

const group = reg.createGroup([box1, box2], { name: 'Pair' });
assert(group.kind === 'group', 'group kind');
assert(group.children.length === 2, 'group has 2 children');
assert(box1.parent === group && box2.parent === group, 'box1/box2 reparented');

const box2WorldAfter = box2.worldCentroid();
assert(dist(box2WorldBefore, box2WorldAfter) < 1e-5,
    `box2 world centroid preserved across grouping ` +
    `(before [${box2WorldBefore}] vs after [${box2WorldAfter}])`);

// Moving the group shifts both children.
group.setTranslation([10, 0, 0]);
const shifted = box2.worldCentroid();
assert(Math.abs(shifted[0] - (box2WorldAfter[0] + 10)) < 1e-5,
    `group move propagates to child (box2 shifted by +10 in x, got ${shifted[0] - box2WorldAfter[0]})`);

// --- explodeGroup unwraps, preserves world transforms ----------------------

const box2WorldGrouped = box2.worldCentroid();
reg.explodeGroup(group);
assert(!reg.getById(group.id), 'group destroyed after explode');
assert(box2.parent === reg.root, 'box2 reparented to root');
const box2WorldExploded = box2.worldCentroid();
assert(dist(box2WorldGrouped, box2WorldExploded) < 1e-5,
    `box2 world centroid preserved across explode`);

// --- Picking promotes to outermost-in-context ------------------------------

reg.clear();
reg._nextId = 1;
const p1 = reg.create({ type: 'box', name: 'p1',
                        position: [0, 0, 0], params: { sx: 1, sy: 1, sz: 1 } });
const g  = reg.createGroup([p1], { name: 'GroupG' });

{
    // From outside the group: picking a face of p1 must return the group
    // as pick.object (innermost-wrapped, outermost-in-context=root).
    const pick = reg.pickAt([0, 5, 0], [0, -1, 0]);
    assert(pick, 'ray hits primitive inside group');
    assert(pick.primitive === p1, 'pick.primitive is the inner leaf');
    assert(pick.object === g, 'pick.object promotes to group');
}

// Enter the group's edit context: picking now returns the primitive.
reg.enterContext(g);
{
    const pick = reg.pickAt([0, 5, 0], [0, -1, 0]);
    assert(pick && pick.primitive === p1, 'pick primitive inside context');
    assert(pick.object === p1, 'pick.object is the primitive when in context');
}
reg.exitContext();
assert(reg.editContext === reg.root, 'exitContext returns to root');

// --- makeComponent + insertComponent ---------------------------------------

reg.clear();
reg._nextId = 1;
const src = reg.create({ type: 'box', name: 'Widget',
                         position: [0, 0, 0], params: { sx: 1, sy: 1, sz: 1 } });
const { definition, instance } = reg.makeComponent([src], { name: 'WidgetComp' });
assert(definition, 'definition created');
assert(instance, 'instance created');
assert(instance.kind === 'component-instance', 'instance is component-instance');
// The definition now owns `src` (it was moved into definition.root).
assert(src.parent === definition.root, 'source primitive under definition root');

// Insert a second instance at +5 X.
const inst2 = reg.insertComponent(definition, { translation: [5, 0, 0] });
assert(inst2 && inst2.definition === definition, 'second instance shares def');

// Picking hits the second instance's shadow primitive at its offset.
{
    const pick = reg.pickAt([5, 5, 0], [0, -1, 0]);
    assert(pick, 'pick hits shadow of instance 2');
    // The shadow is parented under the instance, so outermost-in-root is
    // the instance.
    assert(pick.object === inst2, 'pick.object is the instance');
}

// --- Serialize / deserialize round-trips tree + components -----------------

reg.clear();
reg._nextId = 1;
const a = reg.create({ type: 'box', name: 'A', position: [0,0,0], params:{sx:1,sy:1,sz:1} });
const b = reg.create({ type: 'box', name: 'B', position: [2,0,0], params:{sx:1,sy:1,sz:1} });
const gg = reg.createGroup([a, b], { name: 'ABG' });
gg.setTranslation([10, 0, 0]);
const aWorldBefore = a.worldCentroid();

const serialized = E.serializeScene();
// Nuke and restore.
reg.clear();
assert(reg.primitives.length === 0, 'clear emptied tree');
E.deserializeScene(serialized);
assert(reg.primitives.length === 2, 'restored 2 primitives');
// Find the group in the restored tree.
const restoredGroup = reg.root.children.find(c => c.kind === 'group');
assert(restoredGroup, 'group restored');
assert(restoredGroup.children.length === 2, 'group still has 2 children');
const restoredA = restoredGroup.children.find(c => c.name === 'A');
assert(restoredA, 'primitive A restored');
const aWorldAfter = restoredA.worldCentroid();
assert(dist(aWorldBefore, aWorldAfter) < 1e-5,
    `A world centroid preserved across serialize round-trip`);

console.log('OK — groups: create/explode preserve world, context-aware pick, ' +
            'component instances share definition, serialize round-trip');
