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

  const cuAuto = bro.eye.closeup(scene, { mode: "inventory", isolate: true, showHeading: true, sheetWidth: 768, sheetHeight: 768 });
  assert(cuAuto.ok === true && cuAuto.panelCount >= 2, "inventory closeup works");
  assert(cuAuto.width === 768 && cuAuto.height === 768, "exact native sheet size 768x768");

  // Refractive translucent shader reading scene_color_map & scene_depth_map
  const waterMat = `
    module hook;
    import mesh;
    import scene;
    pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
      var out = s;
      let uv = scene::screen_uv(v.pixel);
      let d = scene::linear_depth(scene::sample_scene_depth(uv));
      let bg = scene::sample_scene_color(uv);
      out.emissive = bg * 0.25 + vec3<f32>(0.0, 0.02 * clamp(d, 0.0, 5.0), 0.03);
      out.alpha = 0.75;
      return out;
    }
  `;
  const pond = scene.createMesh({ name: "Pond", mesh: bro.mesh.box(4, 0.05, 4), color: [0.1, 0.4, 0.5, 0.75] });
  pond.setPosition(0, 0.15, 0);
  pond.setShader({ module: waterMat, file: "water_test.bk" });

  // Lookdev & roughness check
  const ld = bro.eye.lookdev(scene, { target: "Lantern", isolate: true, sheetWidth: 768, sheetHeight: 768 });
  assert(ld.ok === true && ld.width === 768 && ld.height === 768, "lookdev ok & 768x768");
  assert(ld.roughnessMean > 0.2 && ld.roughnessMean <= 1.0, "lookdev measured valid roughness: " + ld.roughnessMean);

  // Dynamic trajectory trace check
  const mover = scene.createMesh({ name: "Falling Leaf", mesh: bro.mesh.sphere(0.1), color: [0.9, 0.3, 0.1] });
  const tr = bro.eye.trace(scene, {
    steps: 10,
    waterY: 0.15,
    minTravel: 0.2,
    sheetWidth: 768,
    sheetHeight: 432,
    stepFn: (k, n) => {
      const f = k / (n - 1);
      mover.setPosition(-1.0 + f * 2.0, 1.2 - f * 1.1, 0.2);
    },
  });
  assert(tr.ok === true && tr.trackCount === 1, "trace tracked Falling Leaf: " + tr.trackCount);
  assert(tr.tracks[0].touchesWater === true, "trace detected water touchdown");
  assert(tr.tracks[0].nearestStartStatic.length > 0, "trace found nearest start static mesh");

  const camAfter = scene.camera.position;
  assert(Math.hypot(camBefore[0] - camAfter[0], camBefore[1] - camAfter[1], camBefore[2] - camAfter[2]) < 1e-4,
         "camera restored after closeup/lookdev/trace");

  print("OK test_eye_closeup: panels=" + cuNamed.panelCount + ", inventory=" + cuAuto.targets.length + ", rough=" + ld.roughnessMean.toFixed(2) + ", trackLen=" + tr.tracks[0].pathLength.toFixed(2));
}
