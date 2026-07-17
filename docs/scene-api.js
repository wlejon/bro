// =============================================================================
// bro Scene Graph API Reference
// =============================================================================
//
// The scene graph API provides 2D and 3D rendering on <canvas> elements.
// Obtain a SceneGraph by calling getContext("scene") on a canvas:
//
//   const canvas = document.querySelector('canvas');
//   const scene = canvas.getContext('scene');
//
// The scene graph supports 2D shapes, sprites, 3D meshes (via bromesh), and
// physics bodies (via Jolt). Nodes form a parent-child hierarchy rooted at
// scene.root. Rendering happens automatically each frame for visible nodes,
// or can be triggered manually via scene.render().
//
// =============================================================================


// -----------------------------------------------------------------------------
// SceneGraph
// -----------------------------------------------------------------------------
// Obtained via `canvas.getContext("scene")`.

class SceneGraph {

  // --- Properties -----------------------------------------------------------

  /** Root node of the scene hierarchy (read-only). All created nodes are added here by default. */
  get root() {}

  /** 2D camera X offset. */
  get cameraX() {}
  set cameraX(value) {}

  /** 2D camera Y offset. */
  get cameraY() {}
  set cameraY(value) {}

  /** 2D camera zoom factor. */
  get cameraZoom() {}
  set cameraZoom(value) {}

  /**
   * Render a kind-specific marker billboard at each LightNode's world
   * position, and include lights in `raycast()` results (as a small
   * world-space sphere). Intended for editor affordances and click-to-
   * select workflows. Off by default.
   */
  get showLightIcons() {}
  set showLightIcons(on) {}

  /**
   * Frustum culling for the forward + shadow passes (default true).
   * Nodes whose conservative world bounds are fully outside the camera
   * frustum are skipped (meshes incl. posed skinned bounds, instanced
   * meshes as a whole node, splats, 3D particles, billboards); shadow
   * casters are culled per light/cascade tile against the LIGHT volume,
   * never the camera, so off-screen casters keep shadowing the view.
   * Culling is strictly conservative — pixels are identical either way —
   * so turning it off is only useful for debugging/regression bisecting.
   * Also settable via `setFrustumCulling(on)`.
   */
  get frustumCulling() {}
  set frustumCulling(on) {}

  /** Method form of the `frustumCulling` property. */
  setFrustumCulling(on) {}

  /**
   * Static shadow-tile cache (default true). Atlas tiles whose light
   * projection and overlapping caster set are unchanged are reused instead
   * of re-rendered — strictly conservative, so pixels are identical either
   * way. See the Shadows section of docs/lighting-api.js for what caches
   * when (spot/point: camera-independent; directional cascades: only while
   * the camera is still; skinned/custom-vertex casters: never).
   * Also settable via `setShadowCache({enabled})`.
   */
  get shadowCache() {}
  set shadowCache(on) {}

  /** Method form of the `shadowCache` property. @param {{enabled:boolean}} opts */
  setShadowCache(opts) {}

  /**
   * Per-category drawn/culled counters from this graph's most recent
   * rendered frame. Shadow counts are per caster x atlas tile; a shadow
   * tile reused by the shadow cache submits no casters (shadowDrawn 0 on a
   * fully cached frame). shadowTilesTotal/Rendered/Cached break down the
   * atlas tiles allocated this frame into re-rendered vs reused. The same
   * counters, summed across all scene graphs, appear as `perf.stats().scene`
   * in headless.
   *
   * @returns {{meshDrawn:number, meshCulled:number,
   *            instancedDrawn:number, instancedCulled:number,
   *            splatDrawn:number, splatCulled:number,
   *            particlesDrawn:number, particlesCulled:number,
   *            billboardsDrawn:number, billboardsCulled:number,
   *            shadowDrawn:number, shadowCulled:number,
   *            shadowTilesTotal:number, shadowTilesRendered:number,
   *            shadowTilesCached:number}}
   */
  cullStats() {}


  // --- Node Creation --------------------------------------------------------

  /**
   * Create a generic SceneNode and add it to the root.
   * @param {string} [name] - optional node name
   * @returns {SceneNode}
   */
  createNode(name) {}

  /**
   * Create a 2D shape node and add it to the root.
   *
   * World-anchored billboards: set `worldAnchor: [x,y,z]` to render the
   * shape inside the 3D mesh FBO as a camera-facing quad (rect / circle
   * SDF), depth-tested against 3D geometry. `billboard: "ylock"` keeps
   * the shape's up axis aligned with world +Y (falls back to full-face
   * when the camera looks nearly straight up/down). Width/height/radius
   * are interpreted in world units when `worldAnchor` is set; the 2D
   * anchorX/anchorY are ignored. Polygon and line shapes are 2D-only.
   *
   * @param {Object} [opts]
   * @param {string} [opts.shape="rect"] - "rect"|"roundrect"|"circle"|"ellipse"|"polygon"|"line"
   * @param {string} [opts.name]
   * @param {number} [opts.width]
   * @param {number} [opts.height]
   * @param {number} [opts.radius] - circle radius
   * @param {number} [opts.radiusX] - ellipse X radius
   * @param {number} [opts.radiusY] - ellipse Y radius
   * @param {number} [opts.cornerRadius] - roundrect corner radius
   * @param {string} [opts.fill] - CSS color string (default: white)
   * @param {string} [opts.stroke] - CSS color string
   * @param {number} [opts.strokeWidth]
   * @param {number} [opts.anchorX=0.5] - anchor point X (0-1, 2D-only)
   * @param {number} [opts.anchorY=0.5] - anchor point Y (0-1, 2D-only)
   * @param {number} [opts.x=0]
   * @param {number} [opts.y=0]
   * @param {number[]} [opts.points] - flat array of [x,y,...] pairs for polygon shape
   * @param {number[]} [opts.worldAnchor] - [x,y,z] world position; switches to 3D billboard
   * @param {string} [opts.billboard="full"] - "full" | "ylock"
   * @returns {SceneNode}
   */
  createShape(opts) {}

  /**
   * Create a 2D sprite node and add it to the root.
   *
   * World-anchored billboards: same options as createShape. The image
   * (or active spritesheet frame's UV sub-rect) uploads to a GL texture
   * and renders inside the 3D mesh FBO, depth-tested against scene
   * geometry. If width/height are unset, the world quad sizes itself
   * from the active sheet frame (or full image) using one world unit
   * per pixel — pass explicit width/height to override.
   *
   * Spritesheet animation: pass `sheet` to slice the source image into
   * frames, then either set `frameIndex` directly or register named
   * `animations` and `play` one. The engine advances frames each
   * frame using the standard frame dt (windowed real time, headless
   * virtual time). When the active animation is non-looping, set
   * `sprite.onAnimationEnd = (name) => {}` to be notified — and
   * optionally chain via the spec's `next` field.
   *
   * @example
   *   scene.createSprite({
   *     src: 'assets/zombie.png',
   *     sheet: { frameWidth: 64, frameHeight: 64, columns: 8, rows: 4 },
   *     animations: {
   *       walk:   { frames: [0,1,2,3,4,5,6,7], fps: 12, loop: true },
   *       attack: { frames: [8,9,10,11],       fps: 16, loop: false, next: "walk" }
   *     },
   *     play: 'walk'
   *   });
   *
   * @param {Object} [opts]
   * @param {string} [opts.name]
   * @param {string} [opts.src] - image file path
   * @param {number} [opts.width] - display width (defaults to sheet frame size or image natural size)
   * @param {number} [opts.height]
   * @param {number} [opts.x=0]
   * @param {number} [opts.y=0]
   * @param {number} [opts.opacity=1.0]
   * @param {number} [opts.anchorX=0.5]
   * @param {number} [opts.anchorY=0.5]
   * @param {Object} [opts.sheet] - spritesheet metadata (grid OR explicit frames)
   * @param {number} [opts.sheet.frameWidth] - per-cell width in image pixels (grid form)
   * @param {number} [opts.sheet.frameHeight]
   * @param {number} [opts.sheet.columns]
   * @param {number} [opts.sheet.rows]
   * @param {Array<{x:number,y:number,w:number,h:number}>} [opts.sheet.frames] - explicit list (replaces grid)
   * @param {Object} [opts.animations] - { name: { frames, fps, loop, next } }
   * @param {string} [opts.play] - initial animation to play
   * @param {number} [opts.frameIndex] - direct frame seek (without animation)
   * @param {number[]} [opts.worldAnchor] - [x,y,z] world position; switches to 3D billboard
   * @param {string} [opts.billboard="full"] - "full" | "ylock"
   * @returns {SceneNode}
   */
  createSprite(opts) {}

  /**
   * Create a 2D particle emitter node. Uses a fixed-size pool (no
   * per-particle allocation after maxParticles is set) and integrates
   * particles in the engine's per-frame tick. Renders via the canvas
   * 2D path and honors the scene's camera transform.
   *
   * `texture` is optional — without it particles render as filled
   * circles using the colour gradient. `blend: "additive"` switches to
   * the canvas-2D `kPlus` blend mode for a glow look (good for sparks,
   * embers).
   *
   * @example
   *   const emitter = scene.createParticles({
   *     x: 100, y: 200,
   *     rate: 50, burst: 30, maxParticles: 500,
   *     lifetime: { min: 0.4, max: 1.0 },
   *     velocity: { angle: -90, angleSpread: 30, speed: 200, speedSpread: 50 },
   *     gravity: { x: 0, y: 600 },
   *     size: { start: 8, end: 0 },
   *     color: { start: '#ffeebb', end: 'rgba(255,180,40,0)' },
   *     blend: 'additive'
   *   });
   *   emitter.burst(50);
   *   emitter.stop();   // existing particles finish
   *   emitter.clear();  // also kill live ones
   *
   * @param {Object} [opts]
   * @param {string} [opts.name]
   * @param {number} [opts.x]
   * @param {number} [opts.y]
   * @param {number} [opts.maxParticles=256] - hard cap on simultaneously alive particles
   * @param {string} [opts.texture] - image path; falls back to filled-circle particles
   * @param {string} [opts.blend="normal"] - "normal" | "additive"
   * @param {number} [opts.rate=0] - emission rate (particles/sec); 0 = burst-only
   * @param {number} [opts.burst=0] - emit N immediately on creation
   * @param {boolean} [opts.autoplay=true]
   * @param {{min:number,max:number}|number} [opts.lifetime] - seconds
   * @param {Object} [opts.velocity]
   * @param {number} [opts.velocity.angle=-90] - center launch angle (degrees)
   * @param {number} [opts.velocity.angleSpread=360] - cone full width (degrees)
   * @param {number} [opts.velocity.speed=100] - center speed (px/s)
   * @param {number} [opts.velocity.speedSpread=0]
   * @param {{x:number,y:number}|number[]} [opts.gravity] - constant accel applied each frame
   * @param {{start:number,end:number}} [opts.size] - particle size at start/end of life
   * @param {{start:string,end:string}|string} [opts.color] - start/end CSS colours (interpolated)
   * @param {Object} [opts.rotation]
   * @param {number} [opts.rotation.start=0] - initial rotation (degrees)
   * @param {number} [opts.rotation.spinSpeed=0] - mean spin (deg/s)
   * @param {number} [opts.rotation.spinSpread=0] - random spread (deg/s)
   * @param {number} [opts.drag=1.0] - per-second velocity multiplier (1 = none, <1 = damping)
   * @returns {SceneNode}
   */
  createParticles(opts) {}

  /**
   * Create a world-space 3D particle emitter node. CPU-simulated on the
   * engine tick (fixed-size pool, deterministic seeded RNG) and rendered as
   * camera-facing instanced billboard quads — one draw call per system —
   * into the HDR 3D pass: particles depth-test against scene geometry
   * (occluded behind walls) without writing depth, and render before
   * tonemap, so `blend: "additive"` systems push HDR luminance and glow
   * for free when bloom is enabled.
   *
   * Without a `texture`, particles are soft round points tinted by the
   * color gradient. `blend: "normal"` sorts the system back-to-front on the
   * CPU each frame; `"additive"` is order-independent (sparks, fire, magic).
   *
   * Simulation space: `space: "world"` (default) keeps particles where they
   * were spawned, so a moving emitter leaves a trail; `space: "local"`
   * integrates in emitter space so the whole cloud rides the node transform
   * (torch flames on a moving character).
   *
   * One-shot systems: give a `duration` (and leave `loop` false) — the
   * system emits for `duration` seconds, drains, and fires `onFinished`
   * exactly once. The callback runs after the node tick, so it may safely
   * destroy the emitter node itself.
   *
   * @example
   *   const fire = scene.createParticles3D({
   *     position: [0, 0.5, 0],
   *     shape: { type: 'cone', radius: 0.15, angle: 12 },
   *     rate: 120, maxParticles: 800, seed: 42,
   *     lifetime: { min: 0.5, max: 1.1 },
   *     velocity: { direction: [0, 1, 0], spread: 10, speed: 1.6, speedSpread: 0.5 },
   *     gravity: [0, 0.6, 0],                     // hot air rises
   *     size: { start: 0.25, end: 0.02 },
   *     color: ['#fff2c0', '#ff8a2a', 'rgba(160,30,10,0)'],
   *     blend: 'additive',
   *   });
   *   fire.burst(40);
   *
   *   const puff = scene.createParticles3D({
   *     shape: 'sphere', burst: 60, rate: 0, duration: 0.1,
   *     lifetime: 0.6, size: { start: 0.3, end: 0.6 },
   *     onFinished: () => puff.destroy(),          // deferred-destroy safe
   *   });
   *
   * @param {Object} [opts]
   * @param {string} [opts.name]
   * @param {number[]} [opts.position] - emitter position [x,y,z] (node transform)
   * @param {number} [opts.maxParticles=256] - hard cap on simultaneously alive particles
   * @param {number} [opts.seed] - RNG seed; a fixed seed + fixed dt steps reproduce exactly
   * @param {string} [opts.texture] - image path; falls back to soft round points
   * @param {{cols:number,rows:number,frames?:number}} [opts.sheet] - flipbook grid on the
   *   texture, played over each particle's lifetime (frames limits to the first N cells)
   * @param {string} [opts.blend="normal"] - "normal" (sorted alpha) | "additive" (glow)
   * @param {number} [opts.softness=0] - soft-particle fade distance in world
   *   units: fragments fade in over this depth gap to the opaque scene behind
   *   them, removing the hard clip line where quads intersect geometry
   *   (smoke hugging a floor, fog against walls). 0 = off. Also a live node
   *   property (`em.softness = 0.5`). Costs one scene-depth copy per frame
   *   while any live system requests it.
   * @param {string|Object} [opts.shape="point"] - emitter shape: "point" | "sphere" |
   *   "hemisphere" | "box" | "cone", or { type, radius, angle, extents:[x,y,z] }
   *   (radius: sphere/hemisphere/cone disc; angle: cone half-angle in degrees;
   *   extents: box half-extents). Sphere/hemisphere launch radially.
   * @param {string} [opts.space="world"] - "world" (trail) | "local" (rides the node)
   * @param {number} [opts.rate=0] - emission rate (particles/sec); 0 = burst-only
   * @param {number} [opts.burst=0] - emit N immediately on creation
   * @param {number} [opts.duration=0] - emission window in seconds; 0 = continuous
   * @param {boolean} [opts.loop=false] - restart the duration window each cycle
   * @param {Function} [opts.onFinished] - one-shot completion callback (window over
   *   AND every particle expired); fires exactly once per play()
   * @param {boolean} [opts.autoplay=true]
   * @param {{min:number,max:number}|number} [opts.lifetime] - seconds
   * @param {Object} [opts.velocity]
   * @param {number[]} [opts.velocity.direction=[0,1,0]] - launch axis (emitter-local)
   * @param {number} [opts.velocity.spread=0] - cone full width around it (degrees)
   * @param {number} [opts.velocity.speed=1] - world units/sec
   * @param {number} [opts.velocity.speedSpread=0]
   * @param {number[]|{x,y,z}} [opts.gravity] - constant world-space acceleration
   * @param {number} [opts.drag=1.0] - per-second velocity multiplier (1 = none)
   * @param {{start:number,end:number}|number} [opts.size] - world-unit quad size over life
   * @param {{start:string,end:string}|string|Array} [opts.color] - start/end CSS colours,
   *   or a gradient array of colours / {t, color} stops over normalized life
   * @param {Object} [opts.rotation] - billboard roll: { start, spinSpeed, spinSpread } (deg)
   * @returns {SceneNode}
   */
  createParticles3D(opts) {}

  /**
   * Create a property Tween — a chainable, engine-ticked animation of node
   * properties (position/rotation/scale/opacity/color), Godot-Tween-flavored.
   * See the Tween class at the bottom of this file for the full surface and
   * a complete example. The tween is owned by the scene graph and advances
   * on the engine frame clock (headless: advanceTime()), so it costs no JS
   * per frame; it persists after finishing (restartable with start()) until
   * tween.destroy().
   * @returns {Tween}
   */
  createTween() {}

  /**
   * Create an HTML-rasterizing scene node and add it to the root.
   *
   * The node owns a detached dom::Document with a root <div> that JS can
   * mutate imperatively via `node.root` (a standard Element wrapper —
   * setInnerHTML, appendChild, textContent, etc). Mutations flag the
   * subtree dirty; the engine re-rasterizes via Skia into an off-screen
   * RGBA buffer and renders as a billboard textured quad through the 3D
   * mesh FBO, depth-tested against scene geometry.
   *
   * Billboarding + worldAnchor behave the same as createShape.
   * Mouse events routed through the engine input pipeline (mousedown,
   * mouseup, click, mousemove, mouseover/out, mouseenter/leave) hit-test
   * against world-space HtmlNode billboards before reaching the canvas
   * itself — listeners attached to elements inside `node.root` fire as
   * if they were rendered in the page. Keyboard input and focus are not
   * routed through HtmlNodes.
   *
   * Example:
   *   const label = scene.createHtmlNode({
   *       width: 240, height: 60,          // CSS pixels (raster surface size)
   *       pxPerUnit: 120,                  // 120px = 1 world unit → 2.0 × 0.5 units
   *       worldAnchor: [0, 3, 0],
   *       billboard: "full",
   *       html: "<div style='color:#fff;font:14px sans-serif'>Hello</div>",
   *   });
   *   label.setHtml("<div style='color:gold'>updated</div>");
   *   label.root.textContent = "mutated directly";  // also re-rasterizes
   *
   * @param {Object} [opts]
   * @param {string} [opts.html] - initial inner HTML of the root div
   * @param {number} [opts.width=200] - raster surface width in CSS pixels
   * @param {number} [opts.height=50] - raster surface height in CSS pixels
   * @param {number} [opts.pxPerUnit=100] - CSS pixels per world unit
   * @param {number[]} [opts.worldAnchor] - [x,y,z] world position
   * @param {string} [opts.billboard="full"] - "full" | "ylock"
   * @param {string} [opts.name]
   * @returns {SceneNode} - also exposes .root (Element) and .setHtml(str)
   */
  createHtmlNode(opts) {}

  /**
   * Create a 3D mesh node and add it to the root.
   * Accepts a Mesh object, named primitive, or raw vertex data.
   *
   * From a Mesh object (see mesh-api.js):
   *   scene.createMesh({ data: Mesh.sphere(1).simplify(0.5), color: 'red' })
   *
   * Named primitives (set opts.mesh to one of these):
   *   "box"      - { halfW=0.5, halfH=0.5, halfD=0.5 }
   *   "sphere"   - { radius=0.5, segments=16, rings=12 }
   *   "cylinder" - { radius=0.5, halfHeight=0.5, segments=16 }
   *   "capsule"  - { radius=0.5, halfHeight=0.5, segments=16, rings=8 }
   *   "plane"    - { halfW=5, halfD=5, subdivX=1, subdivZ=1 }
   *   "torus"    - { majorRadius=1, minorRadius=0.3, majorSegments=24, minorSegments=12 }
   *
   * Raw vertex data (overrides mesh primitive if present):
   *   { positions: Float32Array, indices: Uint32Array, normals?: Float32Array }
   *
   * @param {Object} [opts]
   * @param {Mesh} [opts.data] - a Mesh instance (takes priority over mesh/positions)
   * @param {string} [opts.mesh="box"] - primitive type (ignored if data or positions provided)
   * @param {string} [opts.name]
   * @param {number} [opts.x=0] - position X
   * @param {number} [opts.y=0] - position Y
   * @param {number} [opts.z=0] - position Z
   * @param {number|number[]} [opts.scale=1] - uniform scale, or per-axis
   *   [sx, sy, sz] (missing entries default to 1; normals stay correct
   *   under non-uniform scale)
   * @param {number} [opts.rx=0] - rotation X in degrees
   * @param {number} [opts.ry=0] - rotation Y in degrees
   * @param {number} [opts.rz=0] - rotation Z in degrees
   * @param {string|number[]} [opts.color] - CSS color string or [r,g,b] / [r,g,b,a] (0-1 range)
   * @param {number} [opts.metallic=0] - PBR metallic (0 = dielectric, 1 = metal)
   * @param {number} [opts.roughness=0.7] - PBR roughness (0 = mirror, 1 = diffuse)
   * @param {number} [opts.emissive=0] - emissive intensity (0 = none, >0 = self-lit)
   * @param {string|number[]} [opts.emissiveColor] - emissive tint (defaults to baseColor)
   * @param {Object} [opts.material] - {metallic, roughness} nested form
   * @param {boolean} [opts.twoSided=false] - disable backface culling (leaves, fabric)
   * @param {number} [opts.subsurface=0] - leaf-translucency wrap term [0..1]; takes effect when twoSided is true
   * @param {Float32Array} [opts.positions] - raw vertex positions (xyz, stride 3)
   * @param {Float32Array} [opts.normals] - raw vertex normals (xyz, stride 3)
   * @param {Uint32Array} [opts.indices] - raw triangle indices
   * @param {boolean} [opts.recomputeNormals=false] - raw-data path only:
   *   derive smooth normals from positions+indices when no normals are given
   *   (same flag as updateMesh — see the soft-body recipe in physics-api.js)
   * @returns {SceneNode}
   */
  createMesh(opts) {}

  /**
   * Create a GPU-skinned mesh node and add it to the root. Accepts the full
   * createMesh option surface (primitives make little sense here, so you'll
   * normally pass `data`/`mesh` with a rigged Mesh) plus the skin, and skins
   * on the GPU: positions, normals, AND tangents deform in the vertex shader
   * from a bone-matrix palette, so driving an animation never re-uploads the
   * mesh. Node position/rotation/scale still compose on top, like createMesh.
   * Skinned meshes cast deforming shadows (the shadow pass has a skinned
   * depth-shader variant).
   *
   * The palette holds FINAL skinning matrices — world(bone) * inverseBind —
   * exactly what Pose.computeSkinningMatrices returns (boneCount * 16 floats,
   * column-major mat4). Bone cap: 256 (palette lives in a 16 KB UBO, GL 3.3's
   * guaranteed minimum block size). Until the first setSkinningMatrices the
   * node renders in bind pose.
   *
   * Complete recipe — glTF to animated node (the CPU rigging API computes
   * poses; the GPU node consumes the palettes):
   *
   *   const gltf = Mesh.loadGLTF('character.glb');
   *   const mesh = gltf.meshes[0];
   *   const skin = gltf.skins[0];                       // SkinData
   *   const skel = gltf.skeletons[gltf.meshSkeleton[0]]; // Skeleton
   *   const clip = gltf.animations[0];                   // Animation
   *
   *   const node = scene.createSkinnedMesh({
   *     data: mesh, skin,
   *     color: 'white', roughness: 0.8,
   *   });
   *
   *   // easiest: hand the clip to the built-in animation player — the whole
   *   // evaluate → blend → palette pipeline then runs in C++ every frame
   *   // (see the "Skeletal animation player" section on SceneNode):
   *   node.setSkeleton(skel);
   *   node.addClip('run', clip);
   *   node.play('run', { fadeTime: 0.2 });
   *
   *   // or drive the palette by hand per frame (procedural poses, IK, ...):
   *   let t = 0;
   *   function tick(dt) {
   *     t += dt;
   *     const pose = clip.evaluate(skel, t, { loop: true });
   *     node.setSkinningMatrices(pose.computeSkinningMatrices(skel));
   *   }
   *
   * Procedural rigs work the same way: Rig.autoRig(mesh, spec, landmarks)
   * yields { skeleton, skin } which drop straight in.
   *
   * Physics ragdolls plug into the same palette seam: Physics.createRagdoll
   * gives a rigid-body part tree whose localPose() drops into a bromesh Pose
   * (ragdoll → mesh), and whose driveToPose accepts getBoneWorldMatrix
   * output (animation → ragdoll). Both recipes: docs/physics-api.js,
   * "Ragdoll ↔ skinned mesh recipes".
   *
   * @param {Object} opts - everything createMesh takes, plus:
   * @param {SkinData} opts.skin - (required) per-vertex bone weights/indices
   *        (Mesh.loadGLTF().skins[i], Rig.autoRig().skin, or hand-built).
   *        Vertex count must match the mesh exactly (throws otherwise).
   * @param {Float32Array} [opts.skinningMatrices] - initial palette
   *        (boneCount * 16 floats); defaults to identity = bind pose
   * @returns {SceneNode} - .type === 'skinnedMesh'; exposes .boneCount,
   *          .skinReady, and .setSkinningMatrices(mats)
   */
  createSkinnedMesh(opts) {}

  /**
   * Create a 3D Gaussian Splat node and add it to the root. Renders a splat
   * cloud with EWA splatting (per-splat anisotropic Gaussians, view-dependent
   * SH color, back-to-front sorted). Supply the cloud one of two ways:
   *
   *   // from a .ply on disk (INRIA/3DGS field convention)
   *   scene.createGaussianSplat({ path: "model.ply", scale: 1 });
   *
   *   // from an in-memory cloud — the SoA typed arrays bro.triposplat.generate
   *   // returns drop straight in
   *   const cloud = pipeline.generate(image);
   *   const node = scene.createGaussianSplat({ cloud });
   *   node.savePly("out.ply");        // round-trip it back to disk
   *
   * The node transform applies to the whole cloud: position, rotation, and
   * UNIFORM scale (set at creation via x/y/z/scale or later through the
   * standard node properties) move/orient/resize the splats like any other
   * node. Non-uniform scale is not supported for splat nodes.
   *
   * @param {Object} [opts]
   * @param {string} [opts.path] - .ply to load (resolved app-relative / mounts)
   * @param {Object} [opts.cloud] - in-memory cloud { positions, scales,
   *        rotations, opacities, sh, shDegree } (Float32Array SoA)
   * @param {string} [opts.name]
   * @param {number} [opts.x=0] @param {number} [opts.y=0] @param {number} [opts.z=0]
   * @param {number} [opts.scale=1] - uniform scale
   * @returns {SceneNode} - exposes .splatCount and .savePly(path)
   */
  createGaussianSplat(opts) {}

  /**
   * Configure global wind sway. Per-vertex windBend (vertex color R, 0..1)
   * modulates: pos += direction * sin(time*frequency + dot(pos.xz, k)) * strength * bend.
   * The engine advances `windTime` from the per-frame virtual delta so offline
   * captures stay deterministic.
   * @param {Object} opts
   * @param {number[]} [opts.direction=[1,0,0]] - sway direction [x,y,z]
   * @param {number} [opts.strength=0] - displacement amplitude (world units)
   * @param {number} [opts.frequency=1.5] - oscillation frequency (rad/s)
   */
  setWind(opts) {}

  /**
   * Create a physics body node and add it to the root.
   * @param {Object} [opts]
   * @param {string} [opts.name]
   * @param {number} [opts.body] - Jolt BodyID (raw index+sequence number)
   * @param {number} [opts.pixelsPerUnit=100] - scale factor for physics-to-pixel conversion
   * @param {boolean} [opts.autoSync=true] - automatically sync transform from physics each frame
   * @returns {SceneNode}
   */
  createPhysicsNode(opts) {}


  // --- 3D Camera ------------------------------------------------------------

  /**
   * Configure the 3D camera (perspective or orthographic).
   *
   * Perspective mode (default):
   *   { fov=60, near=0.1, far=1000, aspect, position: [x,y,z], target: [x,y,z], up?: [x,y,z] }
   *
   * Quaternion orientation (avoids target+up precision loss for 6DOF / FPS cameras):
   *   { fov=60, near=0.1, far=1000, aspect, position: [x,y,z], quaternion: [x,y,z,w] }
   *   Camera local -Z is forward. If `quaternion` is set, target/up/mode are ignored.
   *
   * Orthographic mode:
   *   { mode: "orthographic", size=10, near=0.1, far=1000, aspect, position, target, up }
   *
   * @param {Object} opts
   * @param {string} [opts.mode="perspective"] - "perspective" or "orthographic"/"ortho"
   * @param {number} [opts.fov=60] - vertical field of view in degrees (perspective only)
   * @param {number} [opts.size=10] - view height in world units (orthographic only)
   * @param {number} [opts.near=0.1] - near clipping plane
   * @param {number} [opts.far=1000] - far clipping plane
   * @param {number} [opts.aspect] - width/height ratio (defaults to 4/3)
   * @param {number[]} [opts.position=[0,5,-10]] - camera position [x, y, z]
   * @param {number[]} [opts.target=[0,0,0]] - look-at target [x, y, z]
   * @param {number[]} [opts.up=[0,1,0]] - up vector [x, y, z]
   * @param {number[]} [opts.quaternion] - [x,y,z,w] camera orientation (overrides target/up/mode)
   */
  setCamera(opts) {}

  /**
   * Create a dynamic light and add it to the root. Up to 32 visible lights
   * participate in shading per frame. See docs/lighting-api.js for detail.
   *
   * @param {Object} opts
   * @param {string} [opts.type="directional"] - "directional"|"point"|"spot"
   * @param {number[]} [opts.position=[0,0,0]] - world position (point/spot)
   * @param {number[]} [opts.direction=[0,-1,0]] - world direction (dir/spot)
   * @param {string|number[]} [opts.color=[1,1,1]] - linear RGB or CSS string
   * @param {number} [opts.intensity=1.0]
   * @param {number} [opts.range=10] - distance cutoff (point/spot)
   * @param {number} [opts.innerAngle=0.35] - spot full-bright half-angle (radians)
   * @param {number} [opts.outerAngle=0.52] - spot falloff half-angle (radians)
   * @param {string} [opts.name]
   * @returns {SceneNode} LightNode
   */
  createLight(opts) {}

  /**
   * Configure HDR tonemap + exposure applied to the mesh FBO before it is
   * composited onto the 2D backdrop.
   *
   * @param {Object} opts
   * @param {string} [opts.mode="aces"] - "aces"|"reinhard"|"linear"
   * @param {number} [opts.exposure=1.0] - pre-tonemap multiplier
   * @param {number} [opts.gamma=2.2]    - post-tonemap output gamma
   */
  setToneMap(opts) {}

  /**
   * Flat ambient fill added to every fragment (placeholder for IBL).
   * @param {number[]} rgb - linear RGB [r,g,b]
   */
  setAmbient(rgb) {}

  /**
   * Distance fog, applied per-fragment during the forward pass (in linear
   * space, before tonemap) — opaque, translucent, instanced and custom-shader
   * meshes are all fogged consistently; the skybox is never fogged, and fully
   * fogged geometry also fades its alpha so it dissolves into the page
   * background rather than plastering fog color over it.
   *
   * Two modes sharing one `color`:
   *  - Linear ramp (`start`/`end`): fog factor ramps (quadratically) from 0
   *    at `start` to 1 at `end`.
   *  - Exponential-squared + height fog (`density` > 0, overrides the ramp):
   *    factor = 1 - exp(-(density * d)^2) with d = camera distance past
   *    `startDistance`. `heightFalloff` > 0 additionally scales density by
   *    exp(-heightFalloff * worldY), so low-lying geometry sits deeper in
   *    fog than high geometry at the same distance (ground mist).
   *
   * Every call resets both modes' parameters — `setFog({})` disables fog.
   *
   * @param {Object} opts
   * @param {number} [opts.start=0] - linear ramp start distance (world units)
   * @param {number} [opts.end=0]   - linear ramp end distance (world units)
   * @param {number[]} [opts.color=[0,0,0]] - [r,g,b] in 0-1
   * @param {number} [opts.density=0]       - exp^2 fog density (per world unit)
   * @param {number} [opts.heightFalloff=0] - density height decay (per world unit)
   * @param {number} [opts.startDistance=0] - fog-free distance around the camera
   */
  setFog(opts) {}

  /**
   * Screen-space tilt-shift depth-of-field — the "miniature" / toy-model look.
   * Applied as a post pass on the tonemapped LDR frame: a horizontal band stays
   * sharp while the scene blurs toward the top and bottom edges, with an
   * optional saturation/contrast push for the candy-diorama feel. Reads best on
   * a high, distant, looking-down camera. Off by default; pass { enabled: false }
   * (or omit `enabled`) to disable.
   *
   * @param {Object} opts
   * @param {boolean} [opts.enabled=false]    - toggle the whole pass.
   * @param {number}  [opts.focusCenter=0.5]  - sharp-band center, 0 (bottom) → 1 (top).
   * @param {number}  [opts.focusWidth=0.12]  - half-height of the fully-sharp band (UV units).
   * @param {number}  [opts.feather=0.25]     - blur ramp distance past the band edge (UV units).
   * @param {number}  [opts.strength=2.0]     - blur radius multiplier.
   * @param {number}  [opts.saturation=1.0]   - chroma boost (1.0 = unchanged).
   * @param {number}  [opts.contrast=1.0]     - contrast boost (1.0 = unchanged).
   */
  setTiltShift(opts) {}

  /**
   * HDR bloom. Fragments whose HDR luminance exceeds `threshold` bleed a soft
   * glow that's added back in HDR before tonemap, so bright emissives and
   * specular hits bloom filmically. Pair with emissive materials or a bright
   * sun. Off by default (enabled=false leaves the tonemap pass untouched).
   *
   * @param {Object} opts
   * @param {boolean} [opts.enabled=false]   - toggle the bright-pass + blur.
   * @param {number}  [opts.threshold=1.0]   - HDR luminance cutoff (~1.0+).
   * @param {number}  [opts.intensity=0.6]   - additive scale of the glow.
   * @param {number}  [opts.strength=2.0]    - blur radius multiplier.
   */
  setBloom(opts) {}

  /**
   * Screen-space ambient occlusion (SSAO). A half-res AO buffer is computed
   * from the scene depth (hemisphere kernel + rotation noise, then blurred)
   * and multiplied into the lit HDR image before tonemap, darkening creases,
   * corners and contact regions. As a post-multiply on the lit image it
   * affects everything rendered into the 3D FBO (including emissive
   * surfaces) — the standard approach for a forward renderer. Off by
   * default; combines freely with MSAA, bloom, fog and renderScale.
   *
   * @param {Object} opts
   * @param {boolean} [opts.enabled=false] - toggle the pass.
   * @param {number}  [opts.radius=0.5]    - occlusion hemisphere radius (world units).
   * @param {number}  [opts.intensity=1.0] - how dark occlusion gets (0..1+).
   * @param {number}  [opts.bias=0.025]    - depth acceptance offset; raise to
   *   suppress self-occlusion acne on smooth surfaces.
   */
  setSSAO(opts) {}

  /**
   * Depth-based depth-of-field. Geometry within focusDistance ± focusRange
   * (eye-space distance) stays sharp; the circle of confusion ramps to fully
   * defocused by ± 2×focusRange. Applied on the HDR image before bloom and
   * tonemap (defocused highlights still bloom); the defocused image is a
   * half-res Gaussian of the frame (radius maxBlur half-res texels) that the
   * CoC mixes toward — cheap and smooth, same cost tier as tilt-shift. For
   * the screen-space "miniature" band DoF, see setTiltShift; both can be on.
   *
   * @param {Object} opts
   * @param {boolean} [opts.enabled=false]     - toggle the pass.
   * @param {number}  [opts.focusDistance=10]  - distance in perfect focus (world units).
   * @param {number}  [opts.focusRange=5]      - ± span around focus that stays sharp.
   * @param {number}  [opts.maxBlur=4]         - blur radius (half-res texels) when fully defocused.
   */
  setDepthOfField(opts) {}

  /**
   * 3D color-grading LUT, applied in the tonemap pass AFTER tonemapping and
   * gamma (LUTs are authored in display space) with trilinear sampling from
   * a real 3D texture. The LUT is loaded from a horizontal strip image:
   * `size` tiles of size×size laid out left to right, tile index = blue,
   * tile x = red, tile y = green (all increasing left-right / top-down) —
   * e.g. a 16³ LUT is a 256×16 image, the standard neutral-strip layout.
   * A neutral strip is an exact identity. Pass `null` to clear.
   *
   * @param {Object|null} opts - null clears the LUT.
   * @param {string} opts.path - strip image path (png/jpg/bmp/tga), app-relative.
   * @param {number} [opts.size=0] - cube side; 0 infers it from the image height.
   * @param {number} [opts.amount=1.0] - blend between ungraded (0) and graded (1).
   * @returns {boolean} false if the image failed to decode or isn't a
   *   size²×size strip.
   */
  setColorLUT(opts) {}

  /**
   * FXAA 3.11 (quality preset) on the final LDR image — always the LAST
   * pass in the post stack, after tonemap / LUT / tilt-shift. Complements
   * MSAA rather than replacing it: MSAA resolves geometry edges in HDR,
   * FXAA additionally smooths shader/specular/post-pass aliasing on the
   * LDR result — both can be enabled together. Off by default.
   *
   * @param {boolean|Object} enabled - true/false, or {enabled}.
   */
  setFXAA(enabled) {}

  /**
   * Internal render-resolution scale for the 3D pipeline (default 1.0,
   * clamped 0.25-2.0). Every 3D render target (HDR mesh FBO, tonemap,
   * bloom, tilt-shift) is sized to canvas * scale; the compositor samples
   * the result at the CSS element box, so layout, input picking and camera
   * aspect are unaffected. Below 1.0 trades sharpness for fill-rate;
   * above 1.0 supersamples. Note: toImageData()/captureFrame() read the
   * internal target, so their pixel dimensions scale with this.
   *
   * @param {number} s - resolution multiplier (0.25-2.0)
   */
  setRenderScale(s) {}

  /** Current render scale. Also assignable: `scene.renderScale = 0.5`. */
  get renderScale() {}

  /**
   * MSAA for the HDR 3D passes (geometry, splats, particles, billboards).
   * Multisampled color + depth resolve into the single-sampled targets
   * before tonemap, so bloom/tilt-shift, the unlit overlay pass and soft
   * particles all keep working unchanged. Combines with setRenderScale.
   *
   * @param {number} samples - 0 or 1 = off; 2/4/8 typical, clamped to the
   *   driver's GL_MAX_SAMPLES
   */
  setMSAA(samples) {}

  /** Requested MSAA sample count (0 = off). Also assignable: `scene.msaa = 4`. */
  get msaa() {}

  /** Read-only view matrix as 16-element column-major array. */
  get viewMatrix() {}

  /** Read-only projection matrix as 16-element column-major array. */
  get projectionMatrix() {}

  /** Read-only camera world position as [x,y,z]. */
  get cameraEye() {}

  /**
   * Unproject canvas-local pixel coordinates to a world-space ray.
   * `x`/`y` are in CSS pixels relative to the canvas (top-left origin).
   *
   * @param {number} x
   * @param {number} y
   * @returns {?{ origin: number[], dir: number[] }} null if camera uninitialised
   */
  unprojectLocal(x, y) {}


  // --- Queries --------------------------------------------------------------

  /**
   * Find a node by its unique integer ID.
   * @param {number} id
   * @returns {SceneNode|null}
   */
  findById(id) {}

  /**
   * Find a node by name (first match).
   * @param {string} name
   * @returns {SceneNode|null}
   */
  findByName(name) {}


  /**
   * Read the post-tonemap LDR pixels of this scene as an ImageData-shaped
   * object: `{ width, height, data: Uint8ClampedArray }`. Pixels arrive in
   * top-down row order (matches CSS / putImageData), pre-flipped from GL's
   * native bottom-up layout. Alpha is preserved end-to-end, so areas with
   * no 3D content come back as RGBA(0,0,0,0) — letting the result composite
   * cleanly onto a 2D canvas via `ctx2d.putImageData()`.
   *
   * Returns null if no 3D content has been rendered yet (the tonemap FBO is
   * allocated lazily on first 3D pass). Most callers want `captureFrame()`
   * instead — this only returns whatever the engine's last tick produced.
   *
   * @returns {?{ width: number, height: number, data: Uint8ClampedArray }}
   */
  toImageData() {}

  /**
   * Synchronously render the scene and return its tonemap output as an
   * ImageData-shaped object: `{ width, height, data: Uint8ClampedArray }`.
   * Unlike `toImageData()`, this drives the render itself rather than
   * reading whatever the last engine tick happened to produce — so it
   * works in windowed mode (no `flush()` global required) and in any code
   * path that needs a deterministic readback after mutating the scene.
   *
   * Optional `width` / `height` resize the scene's render target before
   * rendering. Sprite-sheet authoring uses this to capture each frame at
   * exactly the cell size, regardless of the host canvas's layout box —
   * the capture canvas can be `display:none`.
   *
   * @example
   *   const scene = canvas.getContext('scene');
   *   scene.createMesh({ mesh: 'box', color: 'red' });
   *   scene.setCamera({ position: [2,2,2], target: [0,0,0] });
   *   const img = scene.captureFrame(64, 64);
   *   sheetCtx.putImageData(img, cellX, cellY);
   *
   * @param {number} [width]  Target render width (omit to use current size)
   * @param {number} [height] Target render height (omit to use current size)
   * @returns {?{ width: number, height: number, data: Uint8ClampedArray }}
   */
  captureFrame(width, height) {}

  /**
   * Live-linked handle to this scene's rendered output (the post-tonemap
   * LDR texture — exactly what the canvas composites), for use as a
   * baseColor map on a mesh in another scene:
   *
   *   const monitor = sceneB.createMesh({ mesh: 'plane', color: '#ffffff' });
   *   monitor.setBaseColorTexture(sceneA.asTexture());   // B shows A, live
   *
   * The link resolves the source texture every frame, so it survives source
   * canvas resizes and renderScale changes (which recreate the underlying
   * GL texture). Lifetime: the handle never keeps the source scene alive —
   * when the source canvas is removed from the document and its scene is
   * destroyed, consuming meshes fall back to their plain `color`. Before
   * the source has rendered its first 3D frame, consumers are likewise
   * untextured.
   *
   * Ordering: scenes render once per frame in canvas getContext('scene')
   * creation order. If the source scene was created before the consumer,
   * the consumer samples this frame's output; otherwise it samples the
   * previous frame's (one frame of latency). There is no reordering API —
   * create the source scene first when same-frame freshness matters.
   *
   * A scene sampling ITSELF (a mesh in A textured with A.asTexture()) is
   * allowed. Lit meshes produce the classic one-frame-delayed recursive
   * "video feedback" image (the lit pass renders into the HDR target, never
   * into the texture being sampled). Unlit meshes draw in a post-tonemap
   * overlay pass directly into the sampled output — a GL feedback loop —
   * so the renderer guards that one case by drawing the mesh untextured
   * (base color) in the overlay pass.
   *
   * @returns {?SceneTexture} handle with a `valid` getter (true while the
   *   source scene still exists)
   */
  asTexture() {}

  /**
   * Cast a ray against all visible 3D mesh nodes in the scene and return the
   * closest hit, or null. Each MeshNode is tested in its local space using
   * the node's pre-built BVH; the ray is transformed by the node's TRS.
   * Non-mesh nodes (shapes, sprites, html, physics nodes) are ignored.
   *
   * Direction need not be unit length — it is normalized internally.
   * `maxDistance` is in world units; pass 0 (or omit) for unlimited range.
   *
   * Example (terrain block picker — aim a ray straight down from above
   * the chunk to find the surface column):
   *   const hit = scene.raycast([cx, chunkH + 20, cz], [0, -1, 0], 200);
   *   if (hit) {
   *       console.log("hit", hit.node.name, "at", hit.position,
   *                   "normal", hit.normal, "dist", hit.distance);
   *   }
   *
   * @param {number[]} origin - [x, y, z] world-space ray origin
   * @param {number[]} direction - [x, y, z] (normalized internally)
   * @param {number} [maxDistance=0] - 0 = unlimited
   * @returns {?{ hit: true, distance: number, position: number[],
   *              point: number[], normal: number[], node: SceneNode }}
   *          `position` and `point` are duplicate aliases (world-space).
   */
  raycast(origin, direction, maxDistance) {}


  // --- Lifecycle ------------------------------------------------------------

  /**
   * Destroy a node and remove it from the scene.
   * @param {SceneNode} node
   */
  destroyNode(node) {}

  /** Sync all physics node transforms from the physics world. */
  syncPhysics() {}


  // --- AI integration (see docs/ai-game-api.js for the full surface) -------

  /**
   * Bind a brogameagent::World and drive it at a fixed step from the engine
   * main loop. After this, each frame: world.tick(1/stepHz) is invoked up to
   * maxStepsPerFrame times based on the real frame delta, then every
   * AgentBinding on this graph steps (think + advance + transform sync).
   *
   * @param {AIWorld} world  - from bro.ai.game.createWorld()
   * @param {Object}  [opts]
   * @param {number}  [opts.stepHz=60]
   * @param {number}  [opts.maxStepsPerFrame=8]
   */
  attachAIWorld(world, opts) {}

  /** Stop auto-ticking the AI world. */
  detachAIWorld() {}
}


// -----------------------------------------------------------------------------
// SceneNode AI extensions
// -----------------------------------------------------------------------------
//
// Any scene node can own an AI binding. See docs/ai-game-api.js for the full
// capability / think API; summary:
//
//   node.attachAgent(world, agent, {
//     capabilities: ["move_to","basic_attack","hold"],
//     thinkHz: 15,
//     think(self, w) { ... },
//   });
//
//   node.detachAgent();
//
// Once attached, the node's x/y/z/rotationY are driven by the agent each
// frame (with yOffset applied on Y for ground clearance).


// -----------------------------------------------------------------------------
// SceneNode
// -----------------------------------------------------------------------------
// Base wrapper for all node types (generic, shape, sprite, mesh, physics).
// Properties specific to a node type (e.g. fillColor on shapes) are silently
// ignored when accessed on other types.

class SceneNode {

  // --- Common Properties ----------------------------------------------------

  /** Unique integer ID (read-only). */
  get id() {}

  /** Node name. */
  get name() {}
  set name(value) {}

  /** Whether this node is visible. */
  get visible() {}
  set visible(value) {}

  /** Parent SceneNode, or null if this is the root or a detached node. */
  get parent() {}

  /** Read-only array of child SceneNodes (snapshot; mutate via add()/remove()). */
  get children() {}

  /** Number of direct children. */
  get childCount() {}


  // --- Transform (all node types) -------------------------------------------

  /** Position X. */
  get x() {}
  set x(value) {}

  /** Position Y. */
  get y() {}
  set y(value) {}

  /** Position Z (3D depth). */
  get z() {}
  set z(value) {}

  /** Rotation around Z axis in radians (legacy 2D shorthand). */
  get rotation() {}
  set rotation(radians) {}

  /** Rotation around X axis in radians. */
  get rotationX() {}
  set rotationX(radians) {}

  /** Rotation around Y axis in radians. */
  get rotationY() {}
  set rotationY(radians) {}

  /** Rotation around Z axis in radians. */
  get rotationZ() {}
  set rotationZ(radians) {}

  /** Scale along X axis (default 1). */
  get scaleX() {}
  set scaleX(value) {}

  /** Scale along Y axis (default 1). */
  get scaleY() {}
  set scaleY(value) {}

  /** Scale along Z axis (default 1). */
  get scaleZ() {}
  set scaleZ(value) {}


  // --- World anchor / billboard (Shape / Sprite / Html) ---------------------

  /**
   * [x,y,z] world-space anchor, or null if not set. When set, the node
   * renders as a camera-facing billboard inside the 3D mesh FBO, depth-
   * tested against 3D geometry, bypassing the 2D canvas path. Assign
   * null to clear and return to 2D rendering.
   */
  get worldAnchor() {}
  set worldAnchor(xyzOrNull) {}

  /** Billboard mode: "full" (face camera) or "ylock" (face camera with +Y up). */
  get billboard() {}
  set billboard(mode) {}


  // --- HtmlNode-only --------------------------------------------------------

  /**
   * Root Element of the HtmlNode's detached DOM subtree. Standard
   * Element API — mutate via innerHTML, appendChild, textContent, etc.
   * Every mutation flips the node dirty; re-rasterization happens on
   * the next frame. Only available on nodes created via createHtmlNode.
   */
  get root() {}

  /**
   * Replace the root's children from an HTML string. Equivalent to
   * setting `root.innerHTML = html` but clearer for the common case.
   */
  setHtml(html) {}

  /** Explicitly mark the HtmlNode dirty (forces a re-raster next frame). */
  markHtmlDirty() {}


  // --- GaussianSplat-only ---------------------------------------------------

  /** Number of splats in the cloud (0 on non-splat nodes). */
  get splatCount() {}

  /**
   * Write the splat cloud to a 3D-Gaussian-Splat .ply (INRIA/3DGS field
   * convention) so it can be reopened here or in any splat viewer. `path` is
   * resolved like createGaussianSplat's path (absolute/drive pass through,
   * leading-slash consults mounts, else app-relative). Throws if the node is
   * not a GaussianSplat, the cloud is empty, or the write fails.
   * @param {string} path
   * @returns {boolean} true on success
   */
  savePly(path) {}


  // --- Shape Properties (ShapeNode only) ------------------------------------

  /** Shape width in pixels. */
  get width() {}
  set width(value) {}

  /** Shape height in pixels. */
  get height() {}
  set height(value) {}

  /** Circle radius. */
  get radius() {}
  set radius(value) {}

  /** Fill color as CSS rgba() string. Set with any CSS color string. */
  get fillColor() {}
  set fillColor(cssColor) {}

  /** Stroke color as CSS rgba() string. Set with any CSS color string. */
  get strokeColor() {}
  set strokeColor(cssColor) {}

  /** Stroke width in pixels. Setting this also enables the stroke. */
  get strokeWidth() {}
  set strokeWidth(value) {}


  // --- Physics Properties (PhysicsNode only) --------------------------------

  /** Whether transform auto-syncs from physics each frame. */
  get autoSync() {}
  set autoSync(value) {}

  /** Pixels-per-unit scale for physics conversion. */
  get pixelsPerUnit() {}
  set pixelsPerUnit(value) {}

  /** Raw Jolt BodyID (read-only, null if no body). */
  get bodyId() {}


  // --- Hierarchy Methods ----------------------------------------------------

  /**
   * Add a child node.
   * @param {SceneNode} child
   * @returns {SceneNode} this (for chaining)
   */
  add(child) {}

  /**
   * Remove a child node.
   * @param {SceneNode} child
   */
  remove(child) {}

  /** Destroy this node (removes from parent and scene). */
  destroy() {}


  // --- SpriteNode animation -------------------------------------------------

  /**
   * For a sprite with a configured sheet, the index of the active frame
   * (0-based into the sheet's frame list). Set directly to seek; reading
   * during animation returns whatever frame is currently displayed.
   */
  get frameIndex() {}
  set frameIndex(value) {}

  /**
   * SpriteNode: true while a registered animation is actively cycling.
   * ParticleNode: true while emitting (existing particles continue
   * either way until they expire).
   */
  get isPlaying() {}

  /** Name of the currently active sprite animation, or "" if none. */
  get currentAnimation() {}

  /**
   * Animation-end callback. Fires once when a non-looping animation
   * completes its last frame, before any chained `next` animation
   * starts. Pass null to clear.
   *   sprite.onAnimationEnd = (name) => { console.log("done:", name); };
   */
  set onAnimationEnd(fn) {}

  /**
   * SpriteNode: start (or resume) an animation. With no argument,
   * resumes the most recently played animation.
   * ParticleNode: resume emission.
   * SkinnedMeshNode: start a registered clip — see the "Skeletal animation
   * player" section below for the options.
   * @param {string} [name]
   * @param {Object} [opts] - skinned mesh only: {loop, speed, fadeTime,
   *        weight, mask}
   * @returns {SceneNode} this
   */
  play(name, opts) {}

  /**
   * SpriteNode: pause animation playback (current frame is held).
   * ParticleNode: stop emitting; existing particles finish naturally.
   * SkinnedMeshNode: fade the player out to bind pose over opts.fadeTime
   * seconds (0 = immediately) and deactivate it — after which manual
   * setSkinningMatrices drives the palette again.
   * @param {{fadeTime?: number}} [opts] - skinned mesh only
   * @returns {SceneNode} this
   */
  stop(opts) {}

  /**
   * Freeze playback in place. SpriteNode: holds the current frame (alias of
   * stop()). SkinnedMeshNode: holds the current pose; the clip clock stops
   * but the player stays active (scrub with `animationTime`, then resume()).
   * @returns {SceneNode} this
   */
  pause() {}

  /**
   * Resume paused playback (SpriteNode / SkinnedMeshNode; ParticleNode
   * resumes emission).
   * @returns {SceneNode} this
   */
  resume() {}

  /**
   * SpriteNode only: register or replace a named animation at runtime.
   * The spec is the same shape as the createSprite `animations` entry.
   * @param {string} name
   * @param {{frames:number[], fps?:number, loop?:boolean, next?:string}} spec
   */
  addAnimation(name, spec) {}


  // --- ParticleNode / Particles3DNode ----------------------------------------

  /** Live particle count (read-only). Alias: `liveCount`. */
  get particleCount() {}

  /** Live particle count (read-only). */
  get liveCount() {}

  /** Steady-state emission rate (particles/sec). Set to 0 for burst-only. */
  get rate() {}
  set rate(value) {}

  /** Emit `n` particles immediately, regardless of `rate`. */
  burst(n) {}

  /** Stop emitting AND kill all live particles immediately. */
  clear() {}

  /**
   * Reconfigure the emitter at runtime. Accepts the same option keys as
   * createParticles / createParticles3D (rate, lifetime, velocity, gravity,
   * size, color, rotation, drag, blend, texture, maxParticles, and for 3D:
   * shape, space, seed, duration, loop, sheet, onFinished).
   * @param {Object} opts
   */
  configure(opts) {}

  /**
   * Particles3DNode: one-shot completion callback (write-only). Fired once
   * per play() when the emission window is over and the last particle has
   * expired; safe to destroy the node from inside it.
   */
  set onFinished(fn) {}


  /**
   * Replace the mesh on a MeshNode in place (keeps transform, color, etc).
   * Accepts the same `data`/`mesh`/`positions+indices` forms as createMesh.
   * No-op on non-mesh nodes.
   *
   * `recomputeNormals: true` (in either argument) derives smooth vertex
   * normals from positions+indices after the swap — the tool for geometry
   * that DEFORMS per frame (a physics soft body streaming `sb.vertices()`
   * in, procedural water, ...) where stale or missing normals render black
   * or faceted. createMesh's raw positions+indices path accepts the same
   * flag. See the soft-body scene-sync recipe in docs/physics-api.js.
   *
   * @param {Object|Mesh} meshOrOpts
   * @param {Object} [opts] - { transfer, recomputeNormals }
   */
  updateMesh(meshOrOpts, opts) {}

  /**
   * Replace or clear the baseColor texture of a MeshNode at runtime. The
   * sample composes with the node's `color` factor (and vertex colors when
   * present), matching glTF `baseColorTexture * baseColorFactor` — set
   * `color` to white for texture pass-through. Three argument forms:
   *
   *   - `{ width, height, data: Uint8Array }` — RGBA8 pixel upload, same
   *     shape as createMesh's `texture` option. The node owns a copy.
   *   - a SceneTexture handle from another scene's `asTexture()` — installs
   *     a LIVE link to that scene's rendered output (see asTexture() for
   *     ordering/lifetime semantics). Non-owning: the source scene keeps
   *     ownership of the texture.
   *   - `null` / `undefined` — clear either form; the mesh falls back to
   *     its plain `color` (and vertex colors if present).
   *
   * The two forms are mutually exclusive: setting one replaces the other.
   *
   * @param {?(Object|SceneTexture)} tex
   */
  setBaseColorTexture(tex) {}


  // --- Custom shaders (MeshNode, SkinnedMeshNode, InstancedMeshNode) ----------
  //
  // ShaderMaterial-style hooks: user GLSL chunks spliced into the engine's
  // mesh uber-shaders, so custom code composes with everything the material
  // system already does — textures, the 32-light PBR loop, shadows, IBL,
  // fog, tonemap. You write GLSL 330 core function bodies, not whole
  // shaders; the engine owns the pipeline around them. One chunk pair works
  // across all three mesh flavours — the engine compiles a program variant
  // per pipeline (static / skinned / instanced) behind the scenes.

  /**
   * Install custom shader chunks on a MeshNode (static or skinned) or an
   * InstancedMeshNode. Chunks are GLSL 330 core fragments spliced into the
   * engine mesh shader at global scope; at least one of `vertex` /
   * `fragment` is required.
   *
   * The vertex chunk must define:
   *
   *   void userVertex(inout vec3 pos, inout vec3 normal, inout vec2 uv)
   *
   * It runs in OBJECT space, after skinning and wind sway and before the
   * camera transforms — displace `pos` and the world position, lighting,
   * fog and shadows all track it. On a skinned mesh the hook receives the
   * POSED position/normal (displacements ride the animation); on an
   * instanced mesh it runs in mesh-local space before the per-instance
   * transform, so the displacement applies identically to every instance in
   * its own frame. `normal` is the object-space normal, `uv` feeds every
   * texture lookup downstream.
   *
   * The fragment chunk must define:
   *
   *   void userFragment(inout vec3 baseColor, inout vec3 normal,
   *                     inout float metallic, inout float roughness,
   *                     inout vec3 emissive, inout float alpha)
   *
   * It runs after ALL material inputs are gathered (base color x texture x
   * vertex tint, MR map, normal map, AO-independent emissive) and before the
   * light loop, so whatever you write into those six values is what standard
   * PBR lighting shades. `normal` is world-space (renormalized after the
   * hook). To output an exact unshaded color, zero `baseColor` and write the
   * color into `emissive`.
   *
   * Both chunks may also:
   *   - declare their own uniforms in the reserved `u_` namespace
   *     (e.g. `uniform vec3 u_tint;`) — set values via the `uniforms` option
   *     or setShaderUniform(). Numeric only: float / vec2 / vec3 / vec4.
   *     Sampler/texture uniforms are NOT supported yet.
   *   - declare custom varyings in the reserved `v_` namespace (an `out` in
   *     the vertex chunk paired with an `in` in the fragment chunk).
   *   - read the engine varyings: vWorldPos (camera-relative world position),
   *     vNormal, vUV, vColor, vCamDist — and engine uniforms like uWindTime.
   *
   * Semantics and limits:
   *   - Compilation happens NOW, at set time — every program variant the
   *     node can render with. Invalid GLSL throws a SyntaxError carrying
   *     the full driver log; the node keeps its previous shader (or the
   *     default pipeline) — nothing half-applies.
   *   - Identical chunk sources across meshes share one compiled program
   *     per pipeline flavour; uniform VALUES stay per-node. Programs are
   *     cached for the scene's lifetime (no eviction).
   *   - A custom shader forces the LIT pass: while one is set, the `unlit`
   *     flag is ignored (unlit meshes normally draw post-tonemap, where the
   *     hook's PBR inputs don't exist). clearShader() restores it.
   *   - Shadows: the depth-only shadow pass runs userVertex too, so a
   *     vertex-displaced MeshNode (static or skinned) casts the DISPLACED
   *     silhouette; fragment-only shaders keep the shared default shadow
   *     program. Exceptions: instanced meshes cast undisplaced shadows,
   *     and a vertex chunk that references mesh-pass-only symbols (e.g. a
   *     varying it declares) falls back to the undisplaced silhouette with
   *     a warning instead of failing.
   *   - Culling: frustum/shadow culling can't see GLSL — a displacement
   *     that pushes geometry outside the mesh's AABB can be culled while
   *     still visible. Set node.cullMargin to the max displacement (world
   *     units) to pad the bounds (same contract as Godot's
   *     extra_cull_margin). On an instanced mesh the hook displaces in
   *     mesh-local space, so scaled instances move displacement x scale in
   *     world units — size cullMargin for the largest instance scale.
   *
   *   // pulse a fresnel-ish rim via a driven uniform
   *   const node = scene.createMesh({ mesh: 'sphere', color: '#334455' });
   *   node.setShader({
   *     fragment: `
   *       uniform float u_pulse;
   *       void userFragment(inout vec3 baseColor, inout vec3 normal,
   *                         inout float metallic, inout float roughness,
   *                         inout vec3 emissive, inout float alpha) {
   *         float rim = pow(1.0 - max(dot(normal, normalize(-vWorldPos)), 0.0), 3.0);
   *         emissive += vec3(0.2, 0.8, 1.0) * rim * u_pulse;
   *       }`,
   *     uniforms: { u_pulse: 1.0 },
   *   });
   *   node.setShaderUniform('u_pulse', 0.25);   // animate per frame
   *
   * @param {Object} opts
   * @param {string} [opts.vertex] - GLSL chunk defining userVertex
   * @param {string} [opts.fragment] - GLSL chunk defining userFragment
   * @param {Object.<string, number|number[]>} [opts.uniforms] - initial
   *        `u_`-prefixed uniform values (number, or array of 1-4 numbers)
   * @returns {SceneNode} this
   * @throws {TypeError} bad argument shapes, non-`u_` uniform names,
   *         non-mesh node
   * @throws {SyntaxError} GLSL compile/link failure (message = driver log)
   */
  setShader(opts) {}

  /**
   * Update one custom-shader uniform on this node. Values are plain numbers
   * (float) or arrays of 1-4 numbers (float/vec2/vec3/vec4) and live on the
   * node — two meshes sharing identical shader source keep independent
   * values. Setting a name the chunk never declares is silently ignored
   * (mirrors GL). Requires a shader installed via setShader.
   * @param {string} name - must use the `u_` prefix
   * @param {number|number[]} value
   * @returns {SceneNode} this
   */
  setShaderUniform(name, value) {}

  /**
   * Remove the custom shader (and its uniform values); the mesh returns to
   * the default pipeline, including its `unlit` behavior if set. The
   * compiled program stays cached in the scene for instant re-use.
   * @returns {SceneNode} this
   */
  clearShader() {}

  /**
   * True while a custom shader is installed on this MeshNode /
   * InstancedMeshNode (read-only).
   */
  get hasShader() {}

  /**
   * Extra world-space padding (units) added to this node's frustum- and
   * shadow-culling bounds. Culling can't see what a custom vertex shader
   * does — set this to the maximum displacement so geometry pushed outside
   * the mesh AABB isn't culled while still visible. 0 by default; Mesh and
   * InstancedMesh nodes only (undefined elsewhere).
   * @type {number}
   */
  cullMargin = 0;


  // --- SkinnedMeshNode-only -------------------------------------------------

  /**
   * Upload the bone palette for a skinned mesh node (created with
   * createSkinnedMesh). `mats` is count * 16 floats of column-major 4x4
   * skinning matrices — Pose.computeSkinningMatrices output drops straight
   * in. Matrices beyond the node's boneCount are ignored; fewer than
   * boneCount updates only the leading bones. Cheap (one memcpy + one UBO
   * sub-upload next frame); this is the per-frame animation hot path — the
   * mesh itself is never re-uploaded.
   * @param {Float32Array} mats
   * @returns {number} matrices actually staged
   */
  setSkinningMatrices(mats) {}

  /** Bone count of the skin palette (0 on non-skinned nodes). */
  get boneCount() {}

  /**
   * True when the skin covers the current mesh (the node renders through the
   * skinned pipeline). Goes false if updateMesh() swaps in a mesh with a
   * different vertex count — the node then draws statically in bind-buffer
   * pose until the mesh matches again.
   */
  get skinReady() {}


  // --- Skeletal animation player (SkinnedMeshNode) ---------------------------
  //
  // The node owns a C++ animation player that runs the whole per-frame
  // pipeline natively — evaluate clip(s) → blend → computeSkinningMatrices →
  // palette — so an app that calls only play() gets animated characters with
  // zero per-frame JS, for any number of characters. It ticks on the engine
  // frame clock (and headless virtual time, so advanceTime() drives it
  // deterministically in tests).
  //
  // Give the node its Skeleton and clips once, then control playback:
  //
  //   const gltf = Mesh.loadGLTF('character.glb');
  //   const node = scene.createSkinnedMesh({ data: gltf.meshes[0],
  //                                          skin: gltf.skins[0] });
  //   node.setSkeleton(gltf.skeletons[gltf.meshSkeleton[0]]);
  //   node.addClip('idle', gltf.animations[0]);
  //   node.addClip('walk', gltf.animations[1]);
  //   node.addClip('wave', gltf.animations[2]);
  //
  //   node.play('idle');                                   // loops by default
  //   node.play('walk', { fadeTime: 0.3 });                // crossfade 0.3 s
  //   node.play('wave', { loop: false, mask: upperBody }); // masked layer on top
  //   node.onAnimationFinished = (name) => node.play('idle', { fadeTime: 0.2 });
  //
  // Model: one BASE track (full-body clip; play() crossfades from whatever
  // was playing) plus one optional masked LAYER track blended on top (e.g.
  // upper-body wave over a walk). Not a state machine or blend tree.
  //
  // Until the first play() — and again after stop() — the player is inactive
  // and manual setSkinningMatrices keeps full control of the palette.

  /**
   * Set the Skeleton the player evaluates clips against (copied — the node
   * keeps its own reference, safe from JS GC). Required before play().
   * @param {Skeleton} skeleton
   * @returns {SceneNode} this
   */
  setSkeleton(skeleton) {}

  /**
   * Register a clip under a name (copied). Replaces same-named clips.
   * @param {string} name
   * @param {Animation} animation - a rigging-API Animation (glTF or hand-built)
   * @returns {SceneNode} this
   */
  addClip(name, animation) {}

  /**
   * Start a clip (see also play/stop/pause/resume above).
   *
   * Without `mask`, the clip takes the BASE track: with fadeTime > 0 the
   * player crossfades from the current blended pose over that many seconds
   * (the outgoing clip keeps advancing while it fades). With `mask`, the
   * clip becomes the LAYER track, blended over the base only on bones whose
   * mask entry is 1 — a one-shot layer expires on finish, a looping layer
   * persists until stop() or a replacement.
   *
   * @param {string} name - a clip registered with addClip (throws otherwise)
   * @param {Object} [opts]
   * @param {boolean} [opts.loop=true] - false: hold the last frame and fire
   *        onAnimationFinished once
   * @param {number} [opts.speed=1] - playback rate (negative plays backward)
   * @param {number} [opts.fadeTime=0] - crossfade seconds (base track only)
   * @param {number} [opts.weight=1] - blend weight; base: vs bind pose,
   *        layer: vs what's underneath
   * @param {Uint8Array|number[]} [opts.mask] - per-bone 0/1, length =
   *        skeleton bone count; non-empty selects the layer track
   * @returns {SceneNode} this
   */
  // play(name, opts) — documented with the shared play() above.

  /** Playback rate multiplier of the base track (get/set). */
  get animationSpeed() {}
  set animationSpeed(value) {}

  /**
   * Base-track clock in seconds (get/set). Setting scrubs: the pose,
   * palette, and getBoneWorldMatrix update immediately, even while paused.
   * Wraps into [0, duration) for looping clips, clamps for one-shots.
   */
  get animationTime() {}
  set animationTime(value) {}

  /** Duration in seconds of the current base clip (0 when none). */
  get animationDuration() {}

  /** Name of the current base clip ("" when none). Shared with sprites. */
  // get currentAnimation() {}

  /** True while the base clip is advancing (not paused / finished). */
  // get isPlaying() {}

  /**
   * Fired once when a non-looping clip (base or layer) reaches its end,
   * with the clip name. Pass null to clear. Safe to play() another clip
   * from inside the callback.
   *   node.onAnimationFinished = (name) => node.play('idle');
   */
  set onAnimationFinished(fn) {}

  /**
   * Current posed matrix of a bone in MODEL space — the skinned mesh's
   * local space, before the node's own position/rotation/scale — as
   * computed by Pose.computeWorldMatrices over the player's current blended
   * pose (bind pose before the first play()). The verification seam for
   * tests and the attachment seam for sockets/props.
   * @param {string|number} boneNameOrIndex
   * @returns {Float32Array|null} 16 floats, column-major, or null if the
   *          bone doesn't exist or no skeleton is set
   */
  getBoneWorldMatrix(boneNameOrIndex) {}


  // --- MeshNode-only --------------------------------------------------------

  /**
   * Fragments closer than this distance are discarded (GL-side clip).
   * Used to hide coarse LOD terrain where finer LODs are loaded. 0 disables.
   */
  get nearClipDist() {}
  set nearClipDist(value) {}

  /** PBR metallic factor (MeshNode only). 0 = dielectric, 1 = metal. */
  get metallic() {}
  set metallic(value) {}

  /** PBR roughness factor (MeshNode only). 0 = mirror, 1 = diffuse. */
  get roughness() {}
  set roughness(value) {}

  /** Emissive intensity scalar (MeshNode only). */
  get emissive() {}
  set emissive(value) {}


  // --- LightNode-only -------------------------------------------------------

  /** [x,y,z] direction vector (directional/spot lights). */
  get direction() {}
  set direction(xyz) {}

  /** [r,g,b] linear color (LightNode only; on other nodes returns undefined). */
  get color() {}
  set color(rgbOrCssString) {}

  /** Radiance multiplier. */
  get intensity() {}
  set intensity(value) {}

  /** Distance cutoff for point/spot lights. */
  get range() {}
  set range(value) {}

  /** Spot inner cone half-angle in radians. */
  get innerAngle() {}
  set innerAngle(radians) {}

  /** Spot outer cone half-angle in radians. */
  get outerAngle() {}
  set outerAngle(radians) {}


  // --- Coordinate Conversion ------------------------------------------------

  /**
   * Transform a local-space point to world space.
   * @param {number} x
   * @param {number} y
   * @param {number} [z=0]
   * @returns {{ x: number, y: number, z: number }}
   */
  localToWorld(x, y, z) {}


  // --- Physics Methods (PhysicsNode only) -----------------------------------

  /** Manually sync this node's transform to its physics body. */
  syncToPhysics() {}
}


// =============================================================================
// Tween — engine-ticked property animation (scene.createTween())
// =============================================================================
//
// A Tween is a SEQUENCE of steps. Each `to()` appends a step; steps run one
// after another; all properties inside one `to()` (and any steps joined with
// `parallel()`) animate simultaneously. `call()` inserts a zero-length
// callback step between animations. The whole sequence can loop. Property
// writes go through the exact same setters the JS API uses, so dirty flags
// and GPU uploads behave as if your code had set node.position itself.
//
// Ticked from the engine frame clock — the same clock as sprite and skeletal
// animation — and under headless virtual time, so advanceTime() drives
// tweens deterministically in tests. Tick overshoot carries across step
// boundaries: timing is independent of frame rate.
//
//   // Slide a crate up with a bounce, flash it red, then fade it out —
//   // looping the whole routine three times:
//   const crate = scene.createMesh({ mesh: 'box', color: 'white' });
//   scene.createTween()
//     .to(crate, { position: [0, 2, 0] }, 0.6, { easing: 'bounceOut' })
//     .to(crate, { color: [1, 0, 0], scale: 1.3 }, 0.2)   // both props together
//     .call(() => console.log('flash!'))
//     .to(crate, { opacity: 0 }, 0.4, { easing: 'quadIn' })
//     .loop(3)
//     .start();
//
//   // Two nodes moving at once: arm the next to() with parallel().
//   scene.createTween()
//     .to(nodeA, { position: [5, 0, 0] }, 1)
//     .parallel()
//     .to(nodeB, { position: [-5, 0, 0] }, 1)
//     .start();
//
//   // Anything else is tweenable through the callback form — the tween
//   // hands you eased progress t in [0..1] every frame:
//   scene.createTween()
//     .to(null, {}, 2, { easing: 'sineInOut', onUpdate: (t) => {
//       scene.setCamera({ position: [Math.sin(t * Math.PI) * 8, 3, 8],
//                         target: [0, 1, 0] });
//     }})
//     .start();
//
// Easing names (bromath's Penner set): 'linear', and In/Out/InOut variants
// of 'quad', 'cubic', 'quart', 'quint', 'sine', 'expo', 'circ', 'back',
// 'elastic', 'bounce' — e.g. 'quadInOut', 'backOut', 'elasticIn'.
// back/elastic overshoot their endpoints mid-curve by design.
class Tween {
  /**
   * Append an animation step (or join the previous step after parallel()).
   *
   * Animatable properties (each optional; all listed animate together):
   *   position    [x,y,z]                       any node
   *   rotation    number (Z radians) | [x,y,z,w] quat | {axis:[x,y,z], angle}
   *   quaternion  [x,y,z,w] (alias of rotation's quat form)
   *   scale       number (uniform) | [x,y,z]    any node
   *   opacity     number 0..1                   Sprite; Mesh (color alpha)
   *   color       [r,g,b] 0..1 | CSS string     Mesh, Light, Shape fill
   *
   * Rotations interpolate as quaternion slerp from the node's rotation at
   * step start. Start values are captured when the step begins (each loop
   * iteration re-captures), so tweens compose from wherever the node
   * currently is. A destroyed node is skipped harmlessly.
   *
   * Pass `null` as the node for a callback-only step: `onUpdate(t)` receives
   * eased progress every tick (t in [0,1]; back/elastic may overshoot) —
   * tween cameras, lights, materials, anything. With neither props nor
   * onUpdate, the step is a pure wait of `duration` seconds.
   *
   * @param {SceneNode|null} node
   * @param {Object} props - see table above (may be {})
   * @param {number} duration - seconds
   * @param {Object} [opts]
   * @param {string} [opts.easing='linear'] - easing name (throws on unknown)
   * @param {number} [opts.delay=0] - seconds to wait inside this step before
   *        these properties start (the step lasts delay + duration)
   * @param {function(number)} [opts.onUpdate] - called with eased t each tick
   * @returns {Tween} this
   */
  to(node, props, duration, opts) {}

  /**
   * Arm parallel merging: the NEXT to() joins the previous animation step
   * instead of appending a new one (they start together; the step ends when
   * the longest member ends). Chain repeatedly for more members.
   * @returns {Tween} this
   */
  parallel() {}

  /**
   * Append a zero-length callback step, fired when the sequence reaches it.
   * Safe to start/stop/destroy this or other tweens from inside.
   * @param {function()} fn
   * @returns {Tween} this
   */
  call(fn) {}

  /**
   * Run the whole sequence `n` times total (loop(2) = play twice). With no
   * argument or Infinity, loops forever. Default is 1 (play once). Each
   * iteration re-captures start values from the nodes' current state.
   * @param {number} [n]
   * @returns {Tween} this
   */
  loop(n) {}

  /** Start (or restart from the beginning). @returns {Tween} this */
  start() {}

  /**
   * Halt and rewind the sequence position (node properties stay wherever
   * they are — nothing snaps back). start() plays again from the top.
   * @returns {Tween} this
   */
  stop() {}

  /** Freeze in place; resume() continues. @returns {Tween} this */
  pause() {}
  resume() {}

  /** True while playing (not paused, not finished/stopped). */
  get isRunning() {}
  get isPaused() {}
  /** True after the last loop iteration completed (not set by stop()). */
  get isFinished() {}

  /**
   * Fired once when the sequence completes its final loop. Not fired by
   * stop() or destroy(). Safe to start() again from inside.
   *   tween.onFinished = () => console.log('done');
   */
  set onFinished(fn) {}

  /**
   * Free the tween. The scene graph keeps finished tweens around so they can
   * be restarted; destroy() releases one you're done with (any later method
   * call on it throws).
   */
  destroy() {}
}
