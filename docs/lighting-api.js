// =============================================================================
// bro Lighting API Reference
// =============================================================================
//
// The scene graph supports forward clustered-style PBR lighting:
//   - Cook-Torrance BRDF (GGX distribution, Schlick Fresnel, Smith geometry)
//   - Up to 32 dynamic lights per frame (directional / point / spot)
//   - Shadow atlas: CSM for directional, 6-face cube for point, spot tiles
//   - Per-mesh PBR material: baseColor, metallic, roughness, emissive, AND
//     normal / metallic-roughness / occlusion textures
//   - Image-based lighting (HDR env → irradiance + GGX prefilter + BRDF LUT)
//   - HDR intermediate render target (RGBA16F) + tonemap pass (ACES by default)
//   - Configurable exposure, gamma, and flat ambient fill
//   - Per-mesh shadow opt-out: `castsShadow`, `receivesShadow`
//
// Lights are SceneNodes — they participate in the hierarchy, transforms, and
// any scene subsystem that operates on nodes (find-by-name, agents, gizmos).
// Their world position comes from the node transform; Directional and Spot
// lights additionally use an explicit `direction` vector.
//
// HDR bloom is available via scene.setBloom and screen-space reflections via
// scene.setSSR (see scene-api.js) — SSR composites local-geometry reflections
// on TOP of the IBL specular described here, weighted by the same
// metallic/roughness material params; a ray miss keeps the IBL reflection.
// Clustered light culling is deferred to a later milestone. See scene-api.js
// for the non-lighting surface, including the tilt-shift DOF post pass.
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
// PBR textures: normal, metallic-roughness, occlusion, emissive
// -----------------------------------------------------------------------------
//
// All four follow the same shape as the baseColor texture: an object with
// { width, height, data: Uint8Array(rgba8) }. Tangents for normal mapping
// are generated automatically from the mesh UVs when a normal map is used.
//
//   normalTexture            — tangent-space normal map (RGBA8, xyz read).
//   metallicRoughnessTexture — glTF packing: G = roughness, B = metallic.
//                              Scalar `metallic` / `roughness` multiply with
//                              the sampled channel, so set scalars to 1.0 to
//                              let the texture drive the value directly.
//   occlusionTexture         — R channel; modulates ambient/IBL only.
//   emissiveTexture          — RGB, multiplied by `emissive` × `emissiveColor`.
//                              Matches glTF's `emission = factor * sample()`:
//                              set `emissive = 1.0` and `emissiveColor` to
//                              the glTF emissiveFactor.
//
// Any combination may be bound; maps you don't set fall back to the scalar
// material params.

scene.createMesh({
    mesh: "sphere",
    radius: 0.5,
    color: "#ffffff",
    metallic:  1.0,                  // treat MR texture as authoritative
    roughness: 1.0,
    texture:                  { width: W, height: H, data: baseColorRGBA },
    normalTexture:            { width: W, height: H, data: normalRGBA    },
    metallicRoughnessTexture: { width: W, height: H, data: mrRGBA        },
    occlusionTexture:         { width: W, height: H, data: aoRGBA        },
    emissiveTexture:          { width: W, height: H, data: emissiveRGBA  },
    emissive: 1.0,
    emissiveColor: [1.0, 0.6, 0.2],  // tint factor from the source file
});


// -----------------------------------------------------------------------------
// Per-mesh shadow flags
// -----------------------------------------------------------------------------
//
// Both default to true. Set either on the createMesh options or at runtime.
//
//   castsShadow    — whether this mesh writes into the shadow atlas. Disable
//                    for foliage impostors, transparent geometry, or meshes
//                    whose silhouette isn't meaningful for shadows.
//   receivesShadow — whether light contributions to this mesh are shadow-
//                    attenuated. Disable for self-lit props or UI billboards
//                    that should stay visible regardless of occluders.
//
// Light-level shadow control is separate: each LightNode has its own
// `castsShadow` flag governing whether that light casts shadows at all.

const grass = scene.createMesh({ mesh: "plane", castsShadow: false });
const hud   = scene.createMesh({ mesh: "sphere", receivesShadow: false });
grass.castsShadow    = false;
hud.receivesShadow   = false;


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

// Ambient is a flat additive term applied after per-light contributions.
// Used as the fallback when no IBL environment is loaded; harmless to
// leave at a small value (0.01-0.05) for indoor scenes that don't want
// to take on a sky tint.
scene.setAmbient([0.03, 0.03, 0.035]);


// -----------------------------------------------------------------------------
// Image-Based Lighting (IBL) — HDR environment maps
// -----------------------------------------------------------------------------
//
// `scene.setEnvironment` loads an HDR equirectangular image (.hdr) and
// uses it for both the visible skybox and the PBR ambient term. Once
// loaded, every metal/dielectric surface picks up the environment's
// colors via Karis's split-sum approximation (a precomputed irradiance
// cube for diffuse + a GGX prefilter mip chain × BRDF LUT for specular).
//
// Loading is synchronous and runs three GPU bake passes off the .hdr:
// equirect→cube, irradiance convolution, and the GGX prefilter chain.
// First call also bakes the env-independent BRDF LUT. Total work is
// ~few hundred million texture taps but happens once per HDR — runtime
// shading is just three texture samples per fragment.

scene.setEnvironment({ hdr: "hdri/venice_sunset_1k.hdr" });

// `intensity` multiplies all IBL contributions (skybox + irradiance +
// prefilter). 1.0 = neutral. Decoupled from sun intensity so you can
// tune them independently.
scene.setEnvironment({ hdr: "/hdri/kiara_dawn_1k.hdr", intensity: 1.5 });

// `rotation` (radians) spins the environment around +Y. Useful for
// aligning the visible sun in the HDR with your directional sun light.
// Affects both the skybox and the IBL sample direction so they stay
// consistent.
scene.setEnvironment({ rotation: Math.PI / 2 });

// Pass any subset of fields to update — omit `hdr` to keep the loaded
// env, omit `intensity`/`rotation` to keep the current values.
scene.setEnvironment({ intensity: 2.0 });

// Pass null (or empty hdr) to clear and fall back to the flat
// `setAmbient` term.
scene.setEnvironment(null);

// Compatible with any Radiance .hdr (top-down, RGBE-encoded). The 12
// CC0 environments under broworkshop's demos/lighting-demo/hdri/ are good starting
// points; more available at polyhaven.com/hdris (all CC0).


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
//      `hit.node` — the same shape mesh hits return. Every scene node
//      exposes a `type` string ('mesh' | 'light' | 'shape' | 'sprite' |
//      'physics' | 'html' | 'group'); lights additionally expose
//      `kind` ('directional' | 'point' | 'spot').
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
//       if (hit && hit.node.type === 'light') {
//           attachGizmoToLight(hit.node, hit.node.kind);
//       }
//   });
//
// For directional lights, translate gizmos are meaningless (position
// doesn't affect shading). Use rotate mode and apply the quaternion delta
// to `node.direction`; for point/spot use translate and bump node.x/y/z.
// See broworkshop's demos/lighting-demo/app.js for a full implementation.


// -----------------------------------------------------------------------------
// Shadows
// -----------------------------------------------------------------------------
//
// Each LightNode can opt into shadow casting. Shadows render into a single
// shared depth atlas (default 4096^2, 16 tiles); the BRDF samples it with
// hardware PCF (default 3x3). All three light kinds are supported:
//
//   directional → 1-4 cascades (CSM), tightly fit per camera-frustum slice
//   spot        → single perspective map covering the cone
//   point       → 6 atlas tiles (cube faces) selected by dominant axis
//
// Atlas budget: 16 tiles. A directional with 4 cascades takes 4 tiles, a
// point takes 6, a spot takes 1. Lights that don't fit silently render
// unshadowed (no error). Plan your budget accordingly:
//   1 directional CSM (4) + 1 point (6) + 6 spots = full atlas.
//
// Per-light shadow controls:
//
//   light.castsShadow       — bool. Default false. Set true to allocate a
//                             tile and start writing this light's shadow.
//   light.shadowBias        — float, default 5e-4. Constant subtracted from
//                             the depth-compare reference. Increase if you
//                             see acne, decrease if peter-panning.
//   light.shadowNormalBias  — float, default 0.03. World-space normal offset
//                             applied to the receiver before sampling. Cheap
//                             curved-surface acne fix; usually leave alone.
//   light.cascadeCount      — int, 1-4 (directional only). Default 4. Each
//                             cascade burns one atlas tile.
//   light.cascadeSplitLambda — 0..1 (directional only). 0 = uniform splits
//                             (indoor), 1 = log splits (outdoor). Default 0.5.
//
// Global controls:
//
//   scene.setShadowQuality(atlasSize, pcfTaps)
//     atlasSize: 1024 / 2048 / 4096 / 8192 (square depth texture side).
//     pcfTaps: 1 (single sample, hard edges) or 3 (3x3 kernel, default).
//
// Static shadow-tile cache (default ON):
//
//   Atlas tiles are only re-rendered when their content could have changed.
//   A tile is reused verbatim when the owning light's shadow projection is
//   unchanged AND the set of shadow casters overlapping the light's frustum
//   is unchanged (no transform/geometry/visibility change, no add/remove).
//   Change detection is conservative-correct — any doubt re-renders — so
//   output pixels are IDENTICAL with the cache on or off; it is pure perf.
//
//   What caches when:
//     spot / point   — camera-independent: fully cached on static scenes,
//                      regardless of camera movement. The big win.
//     directional    — the cascade fit follows the camera, so cascades cache
//                      only while the camera is still (menus, idle scenes,
//                      fixed-camera games).
//     skinned / custom-vertex-shader casters — permanently dynamic: every
//                      tile they overlap re-renders every frame (their pose/
//                      displacement changes without a scene-graph signal).
//
//   Partial invalidation: a single moving caster re-renders only the tiles
//   whose light frustum its bounds overlap — other lights' tiles stay
//   cached. Moving or reconfiguring a light (position, direction, range,
//   outerAngle, cascade count) invalidates that light's tiles only.
//
//   scene.setShadowCache({ enabled })   // or: scene.shadowCache = bool
//     Escape hatch for debugging/bisecting (also gives exact per-frame
//     shadowDrawn counts, since cached tiles submit no casters).
//
//   Per-frame counters in scene.cullStats() / perf.stats().scene:
//     shadowTilesTotal     — atlas tiles allocated this frame
//     shadowTilesRendered  — tiles actually re-rendered
//     shadowTilesCached    — tiles reused from the previous render
//   A fully cached frame reports shadowDrawn 0 (nothing submitted).
//
// Example — sun + a couple of accent lamps casting shadows:
//
//   const sun = scene.createLight({ type:'directional', direction:[-0.4,-1,-0.2] });
//   sun.castsShadow = true;
//   sun.cascadeCount = 4;
//
//   const lamp = scene.createLight({ type:'point', position:[0,3,0],
//                                    range:8, intensity:30 });
//   lamp.castsShadow = true;
//
//   scene.setShadowQuality(2048, 3);   // smaller atlas if VRAM is tight


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
