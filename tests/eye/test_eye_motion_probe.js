function assert(cond, msg) {
  if (!cond) throw new Error("FAIL: " + msg);
}

// 1. Temporal motion analysis on synthetic RGBA8 ImageData frames
{
  const w = 128, h = 64;
  function makeBoxFrame(bx) {
    const data = new Uint8ClampedArray(w * h * 4);
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const i = (y * w + x) * 4;
        const inside = x >= bx && x < bx + 20 && y >= 18 && y < 38;
        data[i + 0] = inside ? 235 : 26;
        data[i + 1] = inside ? 165 : 28;
        data[i + 2] = inside ? 70 : 34;
        data[i + 3] = 255;
      }
    }
    return new ImageData(data, w, h);
  }

  const frames = [0, 14, 28, 42, 56].map(makeBoxFrame);
  const res = bro.eye.motion(frames, {
    labels: ["f0", "f1", "f2", "f3", "f4"],
    timestampsMs: [0, 16.6, 33.2, 85.0, 101.6],
    threshold: 3.0,
    layout: "grid",
  });
  assert(res.frameCount === 5, "motion frameCount === 5");
  assert(res.steps.length === 4, "motion steps.length === 4");
  assert(res.staticPairCount === 0, "linear frames have 0 static pairs");
  assert(res.popCount === 0, "linear frames have 0 pops");
  assert(res.hiccupCount === 1 && res.steps[2].hiccup === true,
         "timestampsMs detects frame pacing hiccup at step 2");
  assert(res.smoothness > 0.7, "linear frames smoothness > 0.7 (got " + res.smoothness + ")");
  assert(res.motionCoverage > 0.05, "motionCoverage > 0.05");
  assert(res.motionBounds.width > 50, "motionBounds spans horizontal sweep");

  for (const mode of ["motion", "chrono", "kymograph"]) {
    const vis = bro.eye.visualize(frames, {
      mode,
      labels: ["f0", "f1", "f2", "f3", "f4"],
    });
    assert(vis && vis.width > 0 && vis.height > 0 && vis.data &&
           vis.data.length === vis.width * vis.height * 4,
           "visualize mode='" + mode + "' returns an RGBA panel sheet");
  }
}

// 2. 3D Spatial calipers via bro.eye.probe(scene, opts)
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

  const leftSphere = scene.createMesh({
    name: "LeftSphere",
    mesh: bro.mesh.sphere(0.6, 24, 16),
    color: [0.85, 0.65, 0.25],
    roughness: 0.3,
    metallic: 0.85,
  });
  leftSphere.setPosition(-1.2, 0.0, 0.0);

  const rightBox = scene.createMesh({
    name: "RightBox",
    mesh: bro.mesh.box(1.0, 1.0, 1.0),
    color: [0.25, 0.55, 0.85],
    roughness: 0.45,
    metallic: 0.2,
  });
  rightBox.setPosition(1.3, 0.0, 0.0);

  const cam = scene.camera;
  cam.setPosition(0.0, 0.2, 5.2);
  cam.lookAt(0.0, 0.0, 0.0);
  scene.render();

  const probe = bro.eye.probe(scene, {
    render: false,
    points: [
      { x: 0.365, y: 0.50, label: "Left" },
      { x: 0.645, y: 0.50, label: "Right" },
      { x: 0.50, y: 0.08, label: "Sky" },
    ],
    pairs: [["Left", "Right"]],
  });

  assert(probe.width === 320 && probe.height === 240, "probe dimensions match scene");
  assert(probe.samples.length === 3, "probe returned 3 point samples");

  const left = probe.samples[0];
  const right = probe.samples[1];
  const sky = probe.samples[2];
  assert(left.hit === true && left.nodeName === "LeftSphere",
         "Left sample hit LeftSphere (got " + left.nodeName + ")");
  assert(right.hit === true && right.nodeName === "RightBox",
         "Right sample hit RightBox (got " + right.nodeName + ")");
  assert(sky.hit === false, "Sky sample missed geometry");
  assert(probe.distances.length === 1, "probe computed 1 pairwise distance");
  assert(probe.distances[0].distance > 1.0 && probe.distances[0].distance < 3.0,
         "Left-Right 3D distance in expected range (got " + probe.distances[0].distance + ")");
}

console.log("PASS: test_eye_motion_probe");
