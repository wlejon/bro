// =============================================================================
// bro Lighting API Reference
// =============================================================================
//
// The scene graph supports forward clustered-style PBR lighting:
//   - Cook-Torrance BRDF (GGX distribution, Schlick Fresnel, Smith geometry)
//   - Up to 32 dynamic lights per frame (directional / point / spot)
//   - Per-mesh PBR material: baseColor, metallic, roughness, emissive
//   - HDR intermediate render target (RGBA16F) + tonemap pass (ACES by default)
//   - Configurable exposure, gamma, and flat ambient fill
//
// Lights are SceneNodes — they participate in the hierarchy, transforms, and
// any scene subsystem that operates on nodes (find-by-name, agents, gizmos).
// Their world position comes from the node transform; Directional and Spot
// lights additionally use an explicit `direction` vector.
//
// Shadows, image-based lighting (IBL), bloom, and PBR textures are deferred
// to later milestones. See scene-api.js for the non-lighting surface.
//
// =============================================================================


// -----------------------------------------------------------------------------
// Lights
// -----------------------------------------------------------------------------

// Directional: infinite-distance key light (sun, moon).
scene.createLight({
    type: "directional",
    direction: [-0.3, -1.0, -0.4],   // pointing FROM light TO scene
    color:     [1.0, 0.98, 0.92],    // linear RGB
    intensity: 3.0,                  // typical sunlight ~3-5
});

// Point: omni-directional falloff.
scene.createLight({
    type: "point",
    position:  [0, 2, 0],
    color:     "#ff8800",
    intensity: 20,
    range:     8,                    // smooth cutoff distance
});

// Spot: directional cone from a position.
scene.createLight({
    type: "spot",
    position:   [0, 5, 0],
    direction:  [0, -1, 0],
    color:      [1, 0.9, 0.6],
    intensity:  40,
    range:      12,
    innerAngle: 0.25,                // fully-lit cone half-angle (radians)
    outerAngle: 0.45,                // falloff edge half-angle
});

// All light properties are mutable via the returned SceneNode:
const lamp = scene.createLight({ type: "point", position: [0,2,0] });
lamp.x = 3;                          // transform (same as any SceneNode)
lamp.color = "#4488ff";              // CSS string or [r,g,b]
lamp.intensity = 15;
lamp.range = 10;


// -----------------------------------------------------------------------------
// PBR materials
// -----------------------------------------------------------------------------
//
// Mesh material uses the glTF metallic/roughness model:
//
//   baseColor   — CSS string or [r,g,b,a]; tinted albedo for dielectrics,
//                 reflectance tint (F0) for metals.
//   metallic    — 0 = dielectric (wood, plastic, skin), 1 = metal.
//   roughness   — 0 = mirror, 1 = fully diffuse. Values below ~0.05 are
//                 clamped in the shader to avoid specular singularities.
//   emissive    — scalar multiplier on emissiveColor; 0 disables emission.
//   emissiveColor — linear RGB tint for self-lit surfaces. Defaults to the
//                 base color when `emissive > 0` is set without a color.

scene.createMesh({
    mesh: "sphere",
    radius: 0.5,
    color: "#c8c8c8",
    metallic: 1.0,
    roughness: 0.2,                  // polished chrome
});

scene.createMesh({
    mesh: "sphere",
    radius: 0.5,
    color: "#d04040",
    metallic: 0.0,
    roughness: 0.8,                  // matte red plastic
});

// Neon-style self-emissive bar:
scene.createMesh({
    mesh: "box",
    halfW: 3, halfH: 0.1, halfD: 0.1,
    emissive: 4.0,
    emissiveColor: [0.5, 0.8, 1.0],
});

// PBR fields are also runtime-mutable on the returned MeshNode:
const s = scene.createMesh({ mesh: "sphere" });
s.metallic = 0.9;
s.roughness = 0.15;
s.emissive = 2.0;


// -----------------------------------------------------------------------------
// Tonemapping, exposure, ambient
// -----------------------------------------------------------------------------
//
// The 3D FBO is rendered at RGBA16F (HDR). Before compositing, it passes
// through a tonemap shader that clamps HDR values to the 0-1 display range.
// ACES is the default (filmic curve, neutral highlights); Reinhard is a
// cheaper approximation; Linear is a raw clamp.
//
// Exposure is a pre-tonemap multiplier — 2.0 = +1 stop brighter. Gamma is
// applied after tonemap for display correction; 2.2 matches sRGB.

scene.setToneMap({ mode: "aces",     exposure: 1.0, gamma: 2.2 });
scene.setToneMap({ mode: "reinhard", exposure: 0.8 });
scene.setToneMap({ mode: "linear" });             // debugging / offline passes

// Ambient is a flat additive term applied after per-light contributions —
// a stand-in until IBL probes land. Keep it small (0.01-0.05); higher
// values wash out the PBR response.
scene.setAmbient([0.03, 0.03, 0.035]);


// -----------------------------------------------------------------------------
// Fallback behavior
// -----------------------------------------------------------------------------
//
// If an app creates 3D geometry but never calls createLight, the scene
// still renders with a single implicit directional "sun" light so meshes
// aren't pitch-black. As soon as any LightNode is added to the graph,
// the implicit sun is dropped and only the explicit lights shade the scene.


// -----------------------------------------------------------------------------
// Editor affordances — light icons + click-to-select
// -----------------------------------------------------------------------------
//
// `scene.showLightIcons = true` enables two behaviors in tandem:
//   1. Each LightNode draws a small kind-specific ringed-disc marker
//      billboard at its world position (directional = large white-ringed
//      disc, point = small colored dot, spot = heavy colored ring).
//      Markers depth-test against geometry, so they occlude correctly.
//   2. `scene.raycast(origin, dir)` also hits those icons as tiny
//      world-space spheres (~0.32 units). Hits return the LightNode as
//      `hit.node` — the same shape mesh hits return. Mesh vs light is
//      disambiguated by `node.type` (or quacking — lights have
//      `intensity`, meshes don't).
//
// Standard click-to-select pattern:
//
//   scene.showLightIcons = true;
//   canvas.addEventListener('pointerdown', ev => {
//       if (bro.gizmo.dragging) return;   // gizmo handles take priority
//       const r = canvas.getBoundingClientRect();
//       const ray = scene.unprojectLocal(ev.clientX - r.left,
//                                        ev.clientY - r.top);
//       const hit = scene.raycast(ray.origin, ray.dir, 100);
//       if (hit && typeof hit.node.intensity === 'number') {
//           attachGizmoToLight(hit.node);
//       }
//   });
//
// For directional lights, translate gizmos are meaningless (position
// doesn't affect shading). Use rotate mode and apply the quaternion delta
// to `node.direction`; for point/spot use translate and bump node.x/y/z.
// See apps/lighting-demo/app.js for a full implementation.


// -----------------------------------------------------------------------------
// Performance notes
// -----------------------------------------------------------------------------
//
// - Budget: 32 active lights per frame. Lights beyond that are silently
//   ignored (first-32-in-node-iteration-order).
// - Forward pass: every pixel loops over every light. Cost scales roughly
//   with (pixels covered) * (light count). For many-light scenes >50,
//   a clustered pass is the next milestone.
// - Spot attenuation uses smoothstep on the cone; inner/outer angles too
//   close together cause a visible hard edge.
// - Range falloff uses the Epic/Frostbite smooth window — lights have
//   physically zero contribution beyond `range`.
