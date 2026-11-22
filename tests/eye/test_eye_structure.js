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

  // Ground floor at y = 0
  scene.createMesh({ name: "Floor", mesh: bro.mesh.box(8, 0.2, 8), color: [0.25, 0.28, 0.32] }).setPosition(0, -0.1, 0);

  // Clean grounded pillar sitting squarely on floor
  const cleanPillar = scene.createMesh({ name: "CleanPillar", mesh: bro.mesh.box(0.6, 1.0, 0.6), color: [0.6, 0.6, 0.6] });
  cleanPillar.setPosition(-1.5, 0.5, 0);

  // Inside-out custom cube mesh (reversed winding indices vs outward normals)
  const badMesh = new Mesh({
    positions: new Float32Array([
      -0.3,-0.3, 0.3,  0.3,-0.3, 0.3,  0.3, 0.3, 0.3, -0.3, 0.3, 0.3,
      -0.3,-0.3,-0.3,  0.3,-0.3,-0.3,  0.3, 0.3,-0.3, -0.3, 0.3,-0.3,
    ]),
    normals: new Float32Array([
      0,0,1, 0,0,1, 0,0,1, 0,0,1,
      0,0,-1, 0,0,-1, 0,0,-1, 0,0,-1,
    ]),
    uvs: new Float32Array(16),
    indices: new Uint32Array([
      0,2,1, 0,3,2,  // front reversed
      4,5,6, 4,6,7,  // back reversed
      0,5,4, 0,1,5,
      3,6,2, 3,7,6,
      1,6,5, 1,2,6,
      0,4,7, 0,7,3,
    ]),
  });
  const invNode = scene.createMesh({ name: "InvertedBox", mesh: badMesh, color: [0.8, 0.3, 0.3] });
  invNode.setPosition(-1.5, 1.3, 0);

  // Floating block hovering 0.45u above CleanPillar
  const fltNode = scene.createMesh({ name: "FloatingCap", mesh: bro.mesh.box(0.4, 0.4, 0.4), color: [0.9, 0.8, 0.3] });
  fltNode.setPosition(0, 1.45, 0);

  // Raised ledge + pillar hanging 50% off its front edge
  scene.createMesh({ name: "Ledge", mesh: bro.mesh.box(1.0, 0.4, 1.0), color: [0.4, 0.5, 0.6] }).setPosition(1.5, 0.2, 0);
  scene.createMesh({ name: "OverhangPost", mesh: bro.mesh.box(0.6, 0.8, 0.6), color: [0.7, 0.5, 0.3] }).setPosition(1.5, 0.8, 0.50);

  // Pair for gap check
  scene.createMesh({ name: "DriverGear", mesh: bro.mesh.box(0.4, 0.4, 0.2), color: [0.8, 0.6, 0.2] }).setPosition(0.0, 0.2, 1.5);
  scene.createMesh({ name: "DrivenGear", mesh: bro.mesh.box(0.4, 0.4, 0.2), color: [0.8, 0.6, 0.2] }).setPosition(0.54, 0.2, 1.5);

  scene.render();

  const st = bro.eye.structure(scene, {
    watchPairs: [["DriverGear", "DrivenGear"]],
  });
  assert(st.ok === false, "structure audit catches defects");
  assert(st.invertedCount >= 1, "detects InvertedBox winding");
  assert(st.floatingCount >= 1, "detects FloatingCap disconnected mesh");
  assert(st.overhangCount >= 1, "detects OverhangPost 50% off ledge");
  assert(st.gapCount >= 1, "detects unmeshed gap between DriverGear and DrivenGear");
}

console.log("PASS: test_eye_structure");
