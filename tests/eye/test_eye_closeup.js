function assert(cond, msg) {
  if (!cond) throw new Error("FAIL: " + msg);
}

if (bro.scene && bro.scene.createGraph) {
  const scene = bro.scene.createGraph(320, 240);
  scene.setAmbient(0.35, 0.35, 0.38);
  const sun = scene.createLight({ name: "Key", type: "directional", color: [1, 1, 1], intensity: 2.5 });
  sun.setDirection(-0.5, -0.7, -0.5);
  scene.camera.setPosition(0, 2.2, 6.0);
  scene.camera.lookAt(0, 0.6, 0);

  scene.createMesh({ name: "Floor", mesh: bro.mesh.box(8, 0.2, 8), color: [0.25, 0.28, 0.32] }).setPosition(0, -0.1, 0);
  scene.createMesh({ name: "Stone Lantern", mesh: bro.mesh.box(0.5, 1.0, 0.5), color: [0.55, 0.58, 0.56] }).setPosition(-1.2, 0.5, 0.2);
  scene.createMesh({ name: "Kohaku Koi", mesh: bro.mesh.sphere(0.35), color: [0.95, 0.45, 0.2] }).setPosition(1.1, 0.35, 0);

  const camBefore = scene.camera.position;

  const cuNamed = bro.eye.closeup(scene, {
    targets: ["Lantern", "Koi"],
    angles: ["threeQuarter", "grazing", "underside"],
    maxPanelDim: 220,
  });
  assert(cuNamed.ok === true, "cuNamed ok === true");
  assert(cuNamed.panelCount === 6, "2 targets x 3 angles = 6 panels, got " + cuNamed.panelCount);
  assert(cuNamed.targets.length === 2, "matched 2 targets");

  const cuAuto = bro.eye.closeup(scene, { maxPanelDim: 200 });
  assert(cuAuto.ok === true && cuAuto.panelCount >= 2, "auto target discovery works");

  const camAfter = scene.camera.position;
  assert(Math.hypot(camBefore[0] - camAfter[0], camBefore[1] - camAfter[1], camBefore[2] - camAfter[2]) < 1e-4,
         "camera restored after closeup capture");

  print("OK test_eye_closeup: panels=" + cuNamed.panelCount + ", autoTargets=" + cuAuto.targets.length);
}
