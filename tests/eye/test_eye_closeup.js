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

  const cuSingle = bro.eye.closeup(scene, { target: "Lantern", angles: ["front", "side"], sheetWidth: 768, sheetHeight: 384 });
  assert(cuSingle.ok === true && cuSingle.panelCount === 2, "singular target alias works");

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

  // Lookdev, material audit & bloom hotspot check
  const ld = bro.eye.lookdev(scene, { target: "Lantern", isolate: true, sheetWidth: 768, sheetHeight: 768 });
  assert(ld.ok === true && ld.width === 768 && ld.height === 768, "lookdev ok & 768x768");
  assert(ld.roughnessMean > 0.2 && ld.roughnessMean <= 1.0, "lookdev measured valid roughness: " + ld.roughnessMean);
  assert(ld.checkedMeshes === 1 && ld.untexturedCount === 1, "lookdev flagged untextured Lantern");
  assert(ld.bloomHotspotCount === 0, "no bloom hotspots in normal light");

  // Multi-state sweep check
  const sw = bro.eye.sweep(scene, {
    states: [
      { label: "Bright", apply: () => { sun.intensity = 2.5; } },
      { label: "Dusk", apply: () => { sun.intensity = 0.6; } },
    ],
  });
  assert(sw.ok === true && sw.stateCount === 2 && sw.width === 768 && sw.height === 432, "sweep captured 2 states at 768x432");

  // Dynamic trajectory trace + snap detection check
  const mover = scene.createMesh({ name: "Falling Leaf", mesh: bro.mesh.sphere(0.1), color: [0.9, 0.3, 0.1] });
  const trSmooth = bro.eye.trace(scene, {
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
  assert(trSmooth.ok === true && trSmooth.trackCount === 1, "trace tracked Falling Leaf: " + trSmooth.trackCount);
  assert(trSmooth.tracks[0].touchesWater === true, "trace detected water touchdown");
  assert(trSmooth.tracks[0].nearestStartStatic.length > 0, "trace found nearest start static mesh");
  assert(trSmooth.tracks[0].snapCount === 0, "smooth ramp has 0 snaps");

  const trSnap = bro.eye.trace(scene, {
    steps: 10,
    waterY: 0.15,
    minTravel: 0.2,
    stepFn: (k, n) => {
      const f = k / (n - 1);
      const jump = k >= 5 ? 0.18 : 0.0;
      mover.setPosition(-1.0 + f * 1.0 + jump, 1.2 - f * 1.1, 0.2);
    },
  });
  assert(trSnap.tracks[0].snapCount >= 1 && trSnap.tracks[0].maxVelJump > 0.1, "trace caught C1 snap");

  // Zero-image per-mesh GPU ID anomaly scanner (diagnose)
  const badCard = bro.mesh.leafCard("oval", { width: 0.8, length: 0.8, fullUV: true });
  const texW = 16, texH = 16;
  const cyanTex = new Uint8Array(texW * texH * 4);
  for (let i = 0; i < texW * texH; ++i) {
    cyanTex[i * 4 + 0] = 200; cyanTex[i * 4 + 1] = 235; cyanTex[i * 4 + 2] = 250; cyanTex[i * 4 + 3] = 255;
  }
  const nrmTex = new Uint8Array(texW * texH * 4);
  for (let i = 0; i < texW * texH; ++i) {
    nrmTex[i * 4 + 0] = 128 + ((i % 2) * 24 - 12); nrmTex[i * 4 + 1] = 128; nrmTex[i * 4 + 2] = 255; nrmTex[i * 4 + 3] = 255;
  }
  // leafCard sets vertex color r=windBend, g=0, b=0 -> red tint_mismatch against pale cyan texture!
  scene.createMesh({
    name: "RedTintedWing",
    mesh: badCard,
    texture: { width: texW, height: texH, data: cyanTex },
    normalTexture: { width: texW, height: texH, data: nrmTex },
    color: [1, 1, 1],
    twoSided: true,
  }).setPosition(0.0, 0.55, 2.2);

  const dg = bro.eye.diagnose(scene, { minPixels: 16 });
  assert(dg.ok === true && dg.visibleMeshes >= 3, "diagnose decoded >= 3 distinct visible GPU mesh IDs: " + dg.visibleMeshes);
  assert(dg.anomalies.some((a) => a.kind === "flat_plastic" && a.nodeName === "Stone Lantern"), "diagnose caught untextured Stone Lantern");
  assert(dg.anomalies.some((a) => a.kind === "tint_mismatch" && a.nodeName === "RedTintedWing"), "diagnose caught vertexColor tint mismatch");

  // Coarse 16-segment twoSided translucent cone -> caught via id_translucent + translucent_striation
  const coarseCone = bro.mesh.cone(0.55, 1.4, 16, 4, false);
  scene.createMesh({
    name: "CoarseBeamCone",
    mesh: coarseCone,
    color: [1.0, 0.85, 0.55, 0.30],
    emissive: 0.35,
    twoSided: true,
    castsShadow: false,
  }).setPosition(1.3, 1.1, 1.8);
  const dgTrans = bro.eye.diagnose(scene, { minPixels: 16 });
  assert(dgTrans.meshes.some((m) => m.name === "CoarseBeamCone" && m.translucent === true), "diagnose attributed translucent CoarseBeamCone");
  assert(dgTrans.anomalies.some((a) => a.kind === "translucent_striation" && a.nodeName === "CoarseBeamCone"),
         "diagnose flagged translucent_striation on coarse 16-seg twoSided cone");

  // Merged mesh with inverted sub-island + floating upper stair step beneath a handrail
  const basePlate = bro.mesh.box(0.5, 0.1, 0.5).translate(3.0, 0.1, 0.0);
  const invIdx = Array.from(bro.mesh.box(0.2, 0.2, 0.2).indices);
  for (let k = 0; k < invIdx.length; k += 3) {
    const tmp = invIdx[k + 1]; invIdx[k + 1] = invIdx[k + 2]; invIdx[k + 2] = tmp;
  }
  const srcInv = bro.mesh.box(0.2, 0.2, 0.2);
  const invBox = new Mesh({
    positions: srcInv.positions, normals: srcInv.normals, uvs: srcInv.uvs, indices: new Uint32Array(invIdx),
  }).translate(3.0, 0.4, 0.0);
  scene.createMesh({ name: "MergedInvertedCorbel", mesh: Mesh.merge([basePlate, invBox]) });

  const step0 = bro.mesh.box(0.3, 0.05, 0.3).translate(-3.2, 0.05, 2.0);
  const step1 = bro.mesh.box(0.3, 0.05, 0.3).translate(-2.6, 0.55, 2.0);
  scene.createMesh({ name: "MergedFloatingStairs", mesh: Mesh.merge([step0, step1]) });
  const railPost = bro.mesh.cylinder(0.03, 0.25, 8).translate(-2.6, 0.85, 2.0);
  const railGround = bro.mesh.cylinder(0.03, 0.42, 8).translate(-3.2, 0.47, 2.0);
  const railBar = bro.mesh.box(0.35, 0.03, 0.03).translate(-2.9, 1.08, 2.0);
  scene.createMesh({ name: "StairHandrail", mesh: Mesh.merge([railPost, railGround, railBar]) });

  const stIsl = bro.eye.structure(scene, { groundY: 0.0, ignoreFloating: ["CoarseBeamCone", "RedTintedWing"] });
  assert(stIsl.invertedCount >= 1 && stIsl.invertedIslandCount >= 1,
         "structure caught inverted sub-island inside merged mesh: " + stIsl.invertedIslandCount);
  assert(stIsl.floatingCount >= 1 && stIsl.floatingIslandCount >= 1,
         "structure caught floating upper stair tread despite top handrail contact: " + stIsl.floatingIslandCount);

  // Surface-hugging BVH conformance check
  const slab = scene.createMesh({ name: "TestCliff", mesh: bro.mesh.box(2.0, 1.0, 0.5) }).setPosition(-3.0, 0.5, 0.0);
  const hugSheet = bro.mesh.plane(1.0, 0.8, 4, 4);
  scene.createMesh({ name: "HugStream", mesh: hugSheet }).setPosition(-3.0, 0.5, 0.258);
  scene.createMesh({ name: "FloatStream", mesh: hugSheet }).setPosition(-3.0, 0.5, 0.65);

  const cf = bro.eye.conformance(scene, {
    pairs: [
      { subject: "HugStream", surface: "TestCliff", maxGap: 0.03 },
      { subject: "FloatStream", surface: "TestCliff", maxGap: 0.05 },
    ],
  });
  assert(cf.pairs.length === 2, "conformance evaluated 2 pairs");
  assert(cf.pairs[0].ok === true && cf.pairs[0].p95Gap < 0.03, "HugStream passes conformance: p95=" + cf.pairs[0].p95Gap);
  assert(cf.pairs[1].ok === false && cf.pairs[1].maxGap > 0.25, "FloatStream fails conformance: max=" + cf.pairs[1].maxGap);
  assert(slab !== undefined, "slab live");

  const camAfter = scene.camera.position;
  assert(Math.hypot(camBefore[0] - camAfter[0], camBefore[1] - camAfter[1], camBefore[2] - camAfter[2]) < 1e-4,
         "camera restored after closeup/lookdev/sweep/trace/diagnose/conformance");

  print("OK test_eye_closeup: panels=" + cuNamed.panelCount + ", inventory=" + cuAuto.targets.length + ", rough=" + ld.roughnessMean.toFixed(2) + ", trackLen=" + trSmooth.tracks[0].pathLength.toFixed(2) + ", diagMeshes=" + dg.visibleMeshes);
}
