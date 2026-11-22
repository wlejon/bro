function assert(cond, msg) {
  if (!cond) throw new Error("FAIL: " + msg);
}

if (bro.scene && bro.scene.createGraph) {
  const scene = bro.scene.createGraph(320, 240);
  scene.setAmbient(0.35, 0.35, 0.38);
  const sun = scene.createLight({
    name: "Key",
    type: "directional",
    color: [1.0, 0.96, 0.9],
    intensity: 2.8,
  });
  sun.setDirection(-0.5, -0.7, -0.5);

  const staticBar = scene.createMesh({
    name: "StaticBar",
    mesh: bro.mesh.box(2.0, 0.3, 0.3),
    color: [0.3, 0.6, 0.85],
  });
  staticBar.setPosition(0, 0, 0);

  const movingRod = scene.createMesh({
    name: "MovingRod",
    mesh: bro.mesh.box(0.3, 2.0, 0.3),
    color: [0.85, 0.5, 0.25],
  });
  movingRod.setPosition(-2.2, 0, 0);

  const cam = scene.camera;
  cam.setPosition(0, 0.8, 6.0);
  cam.lookAt(0, 0, 0);
  scene.render();

  // 1. Separated pose -> no collisions
  const clear = bro.eye.clearance(scene, { eps: 0.003 });
  assert(clear.ok === true && clear.collisions.length === 0,
         "separated meshes pass single-pose clearance");

  // 2. Kinematic sweep where MovingRod slices right through StaticBar mid-sweep
  const sweep = bro.eye.clearance(scene, {
    steps: 8,
    eps: 0.003,
    ignoreStatic: true,
    stepFn: (k, n) => {
      const x = -1.5 + (k / Math.max(1, n - 1)) * 3.0;
      movingRod.setPosition(x, 0, 0);
    },
  });
  assert(sweep.ok === false, "sweep detects rod passing through static bar");
  assert(sweep.dynamicCollisionCount >= 1 && sweep.collisions.length >= 1,
         "sweep records dynamic collision");
  const c0 = sweep.collisions[0];
  const pairMatch = (c0.nodeA === "StaticBar" && c0.nodeB === "MovingRod") ||
                    (c0.nodeA === "MovingRod" && c0.nodeB === "StaticBar");
  assert(pairMatch, "collision names match StaticBar x MovingRod");
  assert(c0.hitTriangles > 0 && Math.abs(c0.centroid[1]) < 0.4,
         "collision reports intersecting triangles near y=0");

  // 3. Explicit ignorePairs excludes intentional contact cleanly
  const ignored = bro.eye.clearance(scene, {
    steps: 8,
    eps: 0.003,
    ignorePairs: [["MovingRod", "StaticBar"]],
    stepFn: (k, n) => {
      const x = -1.5 + (k / Math.max(1, n - 1)) * 3.0;
      movingRod.setPosition(x, 0, 0);
    },
  });
  assert(ignored.ok === true && ignored.collisions.length === 0,
         "ignorePairs suppresses excluded mesh pair cleanly");
}

console.log("PASS: test_eye_clearance");
