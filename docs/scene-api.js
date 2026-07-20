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
// scene.root. Rendering happens automatically each frame for visible nodes —
// there is no manual render() entry point; the only JS-driven render is
// captureFrame(), which renders once off-screen and hands back pixels.
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
   *            decalsDrawn:number, decalsCulled:number,
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
   * Create a clip player for data-driven multi-track keyframe animation of
   * node properties (the Godot AnimationPlayer analog): plain-JSON clips
   * with linear/step/cubic keys, event tracks, loop/pingpong, reverse, and
   * crossfade. Full data model + semantics in docs/animation-api.js.
   * @returns {AnimationPlayer}
   */
  createAnimationPlayer() {}

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
   * @param {boolean} [opts.visible=true] - start hidden with false; omit to
   *   keep the default rather than forcing either state
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
   * @param {boolean} [opts.unlit=false] - skip lighting entirely and output
   *   baseColor (x texture x vertex color). Use for UI/gizmo/debug geometry
   *   and emissive-look surfaces. A custom shader suppresses unlit — see the
   *   custom-shader section for the exact contract.
   * @param {number} [opts.alphaCutoff=0] - alpha test: fragments whose final
   *   alpha is below this `discard`. 0 disables the test. Leaf cards and
   *   other cutout textures want ~0.5.
   * @param {string} [opts.drawMode="triangles"] - "lines"/"line" switches to
   *   GL_LINES (indices are endpoint PAIRS, not triangles) and flips the node
   *   to unlit + non-shadow-casting. Explicit `unlit`/`castsShadow` still win.
   * @param {number} [opts.lineWidth] - GL line width for drawMode 'lines'
   *   (driver-clamped; many drivers only honour 1.0).
   * @param {boolean|number} [opts.wind] - opt into global wind sway
   *   (scene.setWind). true = 1.0, or pass a [0,1] scalar as the whole-mesh
   *   multiplier. Per-vertex bend comes from vertex-color R.
   * @param {boolean} [opts.vertexColorTint] - whether the per-vertex color
   *   stream tints albedo. Unset = auto (tints iff a color buffer exists).
   *   Pass false to keep a color buffer purely as the wind-bend channel, so
   *   foliage sways without being washed by the bend gradient.
   * @param {number|number[]} [opts.depthBias] - polygon offset. A number is
   *   `units`; [factor, units] sets both. Nudges coplanar geometry (decal-ish
   *   overlays, ground markings) out of z-fighting.
   * @param {boolean} [opts.transfer=false] - move the Mesh's buffers into the
   *   node instead of copying them (`data`/`mesh` paths). Faster and
   *   allocation-free for big meshes, but the source Mesh is left empty —
   *   only pass it when you're done with the Mesh.
   * @param {Object} [opts.texture] - base-color (albedo) map, as
   *   { width, height, data: Uint8Array } RGBA8. The node owns a copy.
   * @param {Object} [opts.normalTexture] - tangent-space normal map, same shape
   * @param {Object} [opts.metallicRoughnessTexture] - glTF packed map (G =
   *   roughness, B = metallic), same shape
   * @param {Object} [opts.occlusionTexture] - ambient-occlusion map, same shape
   * @param {Object} [opts.emissiveTexture] - emissive map, same shape
   * @param {Float32Array} [opts.positions] - raw vertex positions (xyz, stride 3)
   * @param {Float32Array} [opts.normals] - raw vertex normals (xyz, stride 3)
   * @param {Float32Array} [opts.colors] - raw per-vertex colors (raw-data path
   *   only); doubles as the wind-bend channel via its R component
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
   * Create an instanced-mesh node and add it to the root: N copies of ONE
   * mesh sharing ONE material, drawn in a single glDrawElementsInstanced
   * call. This is the answer for forests, crowds, debris, bullet swarms,
   * grass — anything where a per-node SceneNode per copy would drown the
   * scene graph. Frustum culling is per-node, not per-instance: the whole
   * batch is drawn or skipped together (cullStats() reports it as
   * instancedDrawn/instancedCulled), so split very large spreads into a few
   * spatially-coherent nodes rather than one global batch.
   *
   * Per-instance state is a 4x3 affine model transform plus an RGBA tint,
   * packed as 16 floats in ROW-major order:
   *
   *   [ m00 m01 m02 tx ]     rows 0-2: the 3x3 basis (rotation x scale)
   *   [ m10 m11 m12 ty ]               with the translation in the last
   *   [ m20 m21 m22 tz ]               column
   *   [ r   g   b   a  ]     row 3: per-instance color
   *
   * The instance color multiplies the material base color in the fragment
   * shader. When an atlas grid is set, alpha instead carries the variant
   * index (see setAtlasGrid). The node's own transform composes on top, so
   * you can move/rotate the whole batch without touching the buffer.
   *
   * Building the buffer by hand is fiddly, so the common path is
   * `instancesFromTransforms`: 9 floats per instance —
   * (px, py, pz, qx, qy, qz, qw, scale, variantIndex) — converted internally
   * to the canonical layout, with RGB defaulting to white and variantIndex
   * packed into alpha as 0..255 → 0..1. Tint individual instances afterwards
   * with updateInstance().
   *
   * @example
   *   // 5000 trees from a single mesh, one draw call:
   *   const xf = new Float32Array(5000 * 9);
   *   for (let i = 0, o = 0; i < 5000; i++, o += 9) {
   *     xf[o] = rand(-200, 200); xf[o+1] = 0; xf[o+2] = rand(-200, 200);
   *     const a = Math.random() * Math.PI * 2;          // yaw only
   *     xf[o+3] = 0; xf[o+4] = Math.sin(a/2); xf[o+5] = 0; xf[o+6] = Math.cos(a/2);
   *     xf[o+7] = rand(0.8, 1.4);                       // scale
   *     xf[o+8] = (i % 4);                              // atlas variant
   *   }
   *   const trees = scene.createInstancedMesh({
   *     mesh: Mesh.loadGLTF('tree.glb').meshes[0],
   *     instancesFromTransforms: xf,
   *     texture: leafAtlas, atlasCols: 2, atlasRows: 2,
   *     alphaCutoff: 0.5, doubleSided: true,
   *     roughness: 0.9,
   *   });
   *   trees.instanceCount;  // 5000
   *
   * @param {Object} [opts]
   * @param {Mesh} opts.mesh - the Mesh instance to replicate (a Mesh handle,
   *   NOT a primitive name — there is no named-primitive path here)
   * @param {Float32Array} [opts.instances] - canonical buffer, 16 floats per
   *   instance; length/16 sets the count
   * @param {Float32Array} [opts.instancesFromTransforms] - convenience
   *   buffer, 9 floats per instance (ignored if `instances` is given)
   * @param {boolean} [opts.transfer=false] - move the Mesh's buffers in
   *   instead of copying (leaves the source Mesh empty)
   * @param {string} [opts.name]
   * @param {number} [opts.x=0] - batch-node position X
   * @param {number} [opts.y=0] - batch-node position Y
   * @param {number} [opts.z=0] - batch-node position Z
   * @param {string|number[]} [opts.color] - material base color; the
   *   per-instance tint multiplies it
   * @param {number} [opts.metallic] - PBR metallic
   * @param {number} [opts.roughness] - PBR roughness
   * @param {number} [opts.emissive=0] - emissive intensity
   * @param {string|number[]} [opts.emissiveColor] - emissive tint (defaults
   *   to the base color when emissive > 0)
   * @param {boolean} [opts.unlit] - skip lighting; suppressed by a custom shader
   * @param {number} [opts.alphaCutoff] - alpha-test threshold (0 = off);
   *   cutout foliage wants ~0.5
   * @param {boolean} [opts.doubleSided] - disable backface culling, so the
   *   back of a leaf card is visible too
   * @param {boolean} [opts.vertexColorTint] - whether the mesh's vertex-color
   *   stream tints albedo
   * @param {boolean} [opts.castsShadow] - include the batch in shadow passes
   * @param {boolean} [opts.receivesShadow] - sample shadows when shading
   * @param {number} [opts.atlasCols=1] - texture-atlas grid columns
   * @param {number} [opts.atlasRows=1] - texture-atlas grid rows; with a grid
   *   set, each instance's alpha selects its atlas cell (see setAtlasGrid)
   * @param {Object} [opts.texture] - base-color map, { width, height,
   *   data: Uint8Array } RGBA8 — same shape as createMesh
   * @param {Object} [opts.normalTexture] - normal map, same shape
   * @param {Object} [opts.metallicRoughnessTexture] - packed MR map, same shape
   * @param {Object} [opts.occlusionTexture] - AO map, same shape
   * @param {Object} [opts.emissiveTexture] - emissive map, same shape
   * @returns {SceneNode} - .type === 'instancedMesh'; see the
   *   "Instanced meshes" section on SceneNode for the runtime surface
   */
  createInstancedMesh(opts) {}

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
   * Create a projected decal (Godot Decal-node analog) and add it to the
   * root. The decal volume is the unit box [-0.5, 0.5]^3 in local space —
   * the node's SCALE is the box size (there is no separate size property;
   * `size` below is just an alias for `scale`), so tweening `scale` animates
   * the decal extent like any node. The decal projects along its local -Y
   * (top-down by default, like Godot); rotate the node to reorient the
   * projection. Texture U maps local +X and V maps local +Z, as seen looking
   * down the projection axis.
   *
   * Per fragment the renderer reconstructs the opaque scene position from
   * the depth buffer (perspective AND ortho cameras), discards outside the
   * box, and alpha-blends `texture * modulate` onto the scene. Decals only
   * appear on OPAQUE geometry (meshes, instanced meshes, terrain, tiles) —
   * translucent meshes, splats, particles and billboards draw over them and
   * never receive them. Decals respect `visible`, `visibilityRange`, and
   * frustum culling like any node.
   *
   *   // blob shadow under a character (no texture = plain modulate tint)
   *   const blob = scene.createDecal({
   *     modulate: [0, 0, 0, 0.5],       // translucent black
   *     size: [1.2, 2, 1.2],            // 1.2x1.2 footprint, 2 units tall
   *     normalFade: 0.3,                // skip walls/cliff faces
   *   });
   *   // parent it under the character so it follows:
   *   hero.add(blob); blob.position = [0, -0.5, 0];
   *
   *   // bullet hole — any { width, height, data: Uint8Array(rgba8) } source
   *   // works (decoded image bytes, canvas readback, procedural), the same
   *   // texture shape createMesh takes:
   *   scene.createDecal({
   *     texture: { width: w, height: h, data: rgbaBytes },
   *     size: [0.4, 0.6, 0.4],
   *     x: hit.x, y: hit.y + 0.3, z: hit.z,   // hover the box over the surface
   *     upperFade: 1, lowerFade: 1,   // soften where the box grazes geometry
   *   });                             // aim with rotation — projects along -Y
   *
   * Lighting (honest limitations vs Godot): Godot's Decal modifies material
   * inputs BEFORE lighting, so decals there are lit exactly like the surface
   * under them. bro's forward renderer has no G-buffer, so decals blend onto
   * the ALREADY-LIT result; to keep them from glowing in shadow they are
   * modulated by a cheap approximation — scene ambient + the dominant
   * directional light's Lambert term (using a screen-space normal
   * reconstructed from depth). Consequences: no per-pixel shadows, IBL, or
   * point/spot light contribution on decals (point/spot-only scenes light
   * decals with ambient alone); no normal/ORM material modification; the
   * reconstructed normal is exact on flat surfaces but faceted on curved
   * ones and noisy at silhouette edges. Emission is exact: it adds straight
   * HDR color (blooms past the threshold like any emissive surface).
   *
   * @param {Object} [opts]
   * @param {string} [opts.name]
   * @param {number} [opts.x=0] @param {number} [opts.y=0] @param {number} [opts.z=0]
   * @param {number|number[]} [opts.size=1] - box size; alias for scale
   *        (uniform number or [x, y, z] world units)
   * @param {number} [opts.rx=0] @param {number} [opts.ry=0] @param {number} [opts.rz=0]
   *        - rotation in degrees (reorients the projection)
   * @param {Object} [opts.texture] - albedo { width, height, data: Uint8Array(rgba8) }
   * @param {Object} [opts.emissionTexture] - self-lit map, same shape
   * @param {string|number[]} [opts.modulate='white'] - tint x master opacity
   *        (CSS color string or [r, g, b, a] floats)
   * @param {number} [opts.emissionStrength=1] - HDR multiplier on emission
   * @param {number} [opts.upperFade=0] - falloff exponent toward the local
   *        +Y end of the box (0 = off; alpha *= pow(1 - t, fade) with t
   *        ramping center -> end)
   * @param {number} [opts.lowerFade=0] - same toward the local -Y end
   * @param {number} [opts.normalFade=0] - cut surfaces facing away from the
   *        projection [0, 1): alpha *= smoothstep(normalFade, 1,
   *        dot(N, projUp) * 0.5 + 0.5). 0 = off.
   * @param {number} [opts.renderPriority=0] - draw order among overlapping
   *        decals (higher = on top; equal keeps creation order)
   * @returns {SceneNode} - .type === 'decal'; live properties `modulate`,
   *          `emissionStrength`, `upperFade`, `lowerFade`, `normalFade`,
   *          `renderPriority`, plus setBaseColorTexture(tex|null) to swap or
   *          clear the albedo at runtime. cullStats() reports
   *          decalsDrawn/decalsCulled. There is no cullMask — the engine has
   *          no mesh-layer concept; use `visible`/`visibilityRange` to gate.
   */
  createDecal(opts) {}

  /**
   * Create a local reflection probe (Godot ReflectionProbe analog) and add
   * it to the root. The probe volume is the unit box [-0.5, 0.5]^3 in local
   * space — the node's SCALE is the box size (`size` is an alias for
   * `scale`, same convention as createDecal) — and the CAPTURE ORIGIN is the
   * node's world position (the box center). Meshes whose bounds center lies
   * inside the box sample the probe's captured surroundings for their
   * SPECULAR ambient term instead of the global IBL environment, so a chrome
   * sphere in a red room reflects the room, not the sky.
   *
   *   // chrome sphere in a red room
   *   const room = buildRedRoom();                  // opaque red walls
   *   const sphere = scene.createMesh({
   *     mesh: Mesh.sphere(1), color: [1, 1, 1, 1],
   *     metallic: 1, roughness: 0.05,
   *   });
   *   scene.createReflectionProbe({
   *     size: 10,                 // box covers the room interior
   *     resolution: 128,
   *     interior: 0.5,            // fade back to global IBL near the walls
   *   });
   *   // default updateMode 'once': the probe captures the room on its first
   *   // visible frame and the sphere turns red. After moving furniture:
   *   //   probe.capture();       // explicit recapture next frame
   *
   * Capture cost & update modes: a capture renders the scene SIX times (cube
   * faces, 90° FOV, at `resolution`) plus a GGX prefilter, so there is
   * deliberately NO per-frame auto mode. 'once' (default) captures on the
   * first rendered frame the probe is visible; 'manual' never captures until
   * probe.capture() asks (each call = one recapture on the next frame).
   * Captures run before the frame's main render, so a capture and its first
   * application land in the same frame.
   *
   * What the capture sees (honest limitations): skybox + OPAQUE meshes
   * (static, skinned, custom-shader, instanced) with full lighting and fog.
   * Excluded: translucent meshes, gaussian splats, 3D particles, billboards,
   * decals, SSR, other probes (no recursion), MSAA, and the post stack
   * (captures are raw HDR — they tonemap with the scene when reflected).
   * Shadows ARE captured: the frame's shadow atlas is reused (spot/point
   * shadows exact; directional cascades stay fitted to the viewer camera, so
   * far-off geometry may capture unshadowed).
   *
   * Application (honest limitations): SPECULAR-ONLY — diffuse ambient stays
   * on the global irradiance / flat ambient, which is also Godot's
   * ReflectionProbe default. ONE probe per mesh draw, selected on the CPU:
   * the highest-`priority` probe whose box contains the mesh's bounds
   * center wins; ties go to the smallest box volume (the more local probe).
   * There is no per-pixel probe blending between overlapping probes — a mesh
   * is either on one probe or on the global environment (the `interior`
   * margin fades per-fragment between its probe and the global IBL near the
   * box faces). SSR composes on top: SSR hits win where the ray march lands,
   * and the probe/IBL result is the natural miss fallback.
   *
   * `boxProjection` (default true — it's the point of a local probe)
   * parallax-corrects the sample: the reflection ray is intersected with the
   * box volume and the hit point is what gets sampled, so flat mirrors and
   * floors line their reflections up with the actual walls. Turn it off for
   * an infinite-distance (skybox-like) sample of the capture.
   *
   * @param {Object} [opts]
   * @param {string} [opts.name]
   * @param {number} [opts.x=0] @param {number} [opts.y=0] @param {number} [opts.z=0]
   * @param {number|number[]} [opts.size=1] - box size; alias for scale
   *        (uniform number or [x, y, z] world units)
   * @param {number} [opts.rx=0] @param {number} [opts.ry=0] @param {number} [opts.rz=0]
   *        - rotation in degrees (orients the box; captures stay world-axis
   *        aligned)
   * @param {number} [opts.resolution=128] - cube face size in texels,
   *        clamped to a power of two in [16, 1024]; changing it later takes
   *        effect on the next capture
   * @param {string} [opts.updateMode='once'] - 'once' | 'manual' (no
   *        per-frame auto mode — see above)
   * @param {boolean} [opts.boxProjection=true] - parallax-correct sampling
   *        against the box volume
   * @param {number} [opts.intensity=1] - multiplier on the probe's specular
   *        contribution
   * @param {number} [opts.interior=0] - blend margin in world units: within
   *        this distance of a box face the probe fades back to the global
   *        IBL (0 = hard edge)
   * @param {number} [opts.priority=0] - selection priority among overlapping
   *        probes (higher wins; ties -> smallest volume)
   * @returns {SceneNode} - .type === 'reflectionProbe'; live properties
   *          `boxProjection`, `intensity`, `interior`, `priority`,
   *          `resolution`, `updateMode`, plus .capture() to request a
   *          recapture. GPU cubemaps are freed with the node (destroy()
   *          restores the global environment for affected meshes).
   */
  createReflectionProbe(opts) {}

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
   * @param {number} [opts.pixelsPerUnit=1] - scale factor for physics-to-scene
   *   conversion: the body's position is multiplied by this on every sync.
   *   Leave at 1 for a 3D scene in physics units; set it to e.g. 50 for a 2D
   *   scene laid out in pixels against a metres-based physics world.
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
   * @param {number} [opts.aspect] - width/height ratio. Omit it (or pass <= 0)
   *   and the projection is built from the current canvas size AND flagged to
   *   auto-follow future canvas resizes — normally what you want. An explicit
   *   aspect pins the projection and disables the follow behavior. With no
   *   canvas size yet, the omitted case falls back to 4/3.
   * @param {number[]} [opts.position=[0,5,-10]] - camera position [x, y, z]
   * @param {number[]} [opts.target=[0,0,0]] - look-at target [x, y, z]
   * @param {number[]} [opts.up=[0,1,0]] - up vector [x, y, z]
   * @param {number[]} [opts.quaternion] - [x,y,z,w] camera orientation (overrides target/up/mode)
   *
   * Precedence vs camera NODES (createCamera/setActiveCamera): the LAST
   * camera call wins — calling setCamera (any variant) deactivates the
   * active camera node and installs this imperative view; setActiveCamera
   * overrides an imperative view. `scene.activeCamera` is null while the
   * imperative view is in effect.
   */
  setCamera(opts) {}

  /**
   * Create a camera NODE (Godot Camera3D analog) and add it to the root.
   * Only projection parameters live on the node — the VIEW is the node's
   * world transform: the camera looks down its local -Z axis with local +Y
   * up, so parent it under a vehicle/character node and it inherits that
   * motion like any other node. Position/orient it with the normal node
   * transform surface (position, quaternion, node.lookAt()), animate it with
   * tweens; while active, the engine derives the view from its world matrix
   * every tick right after animations/tweens run (and again at render), so a
   * tweened or parented camera is smooth with zero per-frame JS.
   *
   * ALL camera-state consumers follow the active camera: shadow cascades
   * (CSM near/far fitting), frustum culling, billboards, soft particles,
   * depth of field, the skybox, unprojectLocal/picking, readTonemap
   * readbacks (toImageData/captureFrame), and a listener bound with
   * bindAudioListenerToCamera.
   *
   * Aspect follows the canvas through resizes unless `aspect` is set (> 0)
   * — same behavior as setCamera's omitted-aspect mode.
   *
   * Projection params are live-editable on the node afterwards: `fov`
   * (degrees), `near`, `far`, `aspect`, `size`, `projection`. Tween a zoom
   * with a callback-only tween step (see Tween.to):
   *   scene.createTween().to(null, {}, 0.5,
   *       { onUpdate: t => { cam.fov = 60 - 30 * t; } }).start();
   *
   * @param {Object} [opts]
   * @param {string} [opts.name]
   * @param {string} [opts.mode="perspective"] - "perspective" or "orthographic"/"ortho"
   * @param {number} [opts.fov=60] - vertical field of view in degrees (perspective)
   * @param {number} [opts.size=10] - view height in world units (orthographic)
   * @param {number} [opts.near=0.1] - near clipping plane
   * @param {number} [opts.far=1000] - far clipping plane
   * @param {number} [opts.aspect=0] - explicit aspect; <= 0 follows the canvas
   * @param {number[]} [opts.position=[0,0,0]] - node-local position [x, y, z]
   * @param {number[]} [opts.quaternion] - [x,y,z,w] node-local orientation
   * @param {number[]} [opts.lookAt] - world-space target to aim at (ignored when quaternion is set)
   * @param {boolean} [opts.active=false] - immediately setActiveCamera(this)
   * @returns {SceneNode} CameraNode (node.type === "camera")
   * @example
   *   const cam = scene.createCamera({ position: [0, 2, 8], lookAt: [0, 0, 0], active: true });
   *   const chase = scene.createCamera({ position: [0, 3, -6], lookAt: [0, 0, 20] });
   *   vehicle.add(chase);                 // follows the vehicle
   *   scene.setActiveCamera(chase);       // switch views
   */
  createCamera(opts) {}

  /**
   * Activate a camera node created with createCamera(): its world transform
   * drives the view from now on. Pass null to deactivate (the last derived
   * view is kept until the next camera call). Any imperative setCamera()
   * call also deactivates — last camera call wins.
   * @param {SceneNode|null} cameraNode
   */
  setActiveCamera(cameraNode) {}

  /**
   * The active camera node, or null while the imperative setCamera() view is
   * in effect (or after the active node was destroyed). Writable — assigning
   * is equivalent to setActiveCamera(), null included.
   *
   * Node wrappers have stable identity, so this compares equal to the node you
   * activated: `scene.activeCamera === myCam` is true, and scene nodes work as
   * Set/Map keys and with indexOf. No need to compare by .name.
   * @type {SceneNode|null}
   */
  get activeCamera() {}
  set activeCamera(cameraNode) {}

  /**
   * Bind the 3D audio listener to this scene's camera. While bound, the
   * engine pushes the camera's world position, orientation (forward/up from
   * the view matrix), and velocity (finite difference over the scaled frame
   * dt — feeds Doppler, see audio-api.js) into the broaudio listener every
   * frame, after animations/tweens run. Zero per-frame JS; replaces manual
   * ctx.setListenerPosition/Orientation/Velocity calls.
   *
   * One listener exists engine-wide: bind it from ONE scene (binding from
   * several makes the last-iterated scene win). The binding dies with the
   * scene graph; pass false to unbind explicitly. Pairs with
   * node.attachAudioEmitter() for fully automatic 3D audio.
   * @param {boolean} [enabled=true]
   * @example
   *   scene.bindAudioListenerToCamera(true);
   *   scene.setCamera({ position: [0, 2, 8], target: [0, 0, 0] });  // audio follows
   */
  bindAudioListenerToCamera(enabled) {}

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
   * Screen-space reflections (SSR) on opaque surfaces. A full-screen pass
   * runs right after the opaque + decal passes: for every opaque pixel the
   * reflected view ray is marched against the scene depth buffer (linear
   * steps + binary refinement), and on a hit the HDR color at the hit pixel
   * is blended over the surface. The blend weight is a per-pixel
   * reflectance mask derived from the material — luminance of F0
   * (normal-incidence Fresnel: 0.04 for dielectrics, up to the base-color
   * luminance for metals) x (1 - roughness)^2 — times `intensity`, so
   * smooth metals mirror, rough or dielectric surfaces reflect faintly, and
   * unlit surfaces not at all. Off by default; works with perspective AND
   * orthographic cameras, MSAA, renderScale and the rest of the post stack.
   *
   * SSR composites ON TOP of image-based lighting, it does not replace it:
   * a miss changes nothing, leaving the IBL specular (or flat ambient) as
   * the fallback reflection. Honest limitations of any screen-space
   * technique apply: only what is on screen can be reflected (objects
   * offscreen, behind the camera, or occluded reflect nothing — the miss
   * falls back to IBL), only opaque geometry reflects and is reflected
   * (translucents, splats, particles and billboards draw over reflections
   * and never appear in them), and reflections use the front-face colors
   * the camera sees. Reflections fade near screen borders (`edgeFade`) and
   * for rays bending back toward the camera to hide those artifacts.
   *
   * @param {Object} opts
   * @param {boolean} [opts.enabled=false]   - toggle the pass.
   * @param {number}  [opts.maxDistance=30]  - max reflected-ray length in
   *   world units.
   * @param {number}  [opts.steps=48]        - linear march steps across
   *   maxDistance (a few binary-refine steps sharpen each hit); more steps
   *   catch thinner geometry at higher cost. Clamped to 4..256.
   * @param {number}  [opts.thickness=0.3]   - view-space depth tolerance: a
   *   ray sample counts as a hit when it sits at most this far behind the
   *   depth buffer. Raise to catch thin ledges; lower to reduce false hits
   *   at silhouettes.
   * @param {number}  [opts.intensity=1.0]   - scales the reflection weight
   *   (0..1+; the final weight is clamped to 1).
   * @param {number}  [opts.edgeFade=0.1]    - screen-border fade width as a
   *   fraction of the viewport (0..0.5); hits closer to an edge than this
   *   fade out linearly.
   */
  setSSR(opts) {}

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

  /**
   * Pull physics-node transforms from the physics world (position scaled by
   * each node's `pixelsPerUnit`, rotation copied as-is). Nodes with
   * `autoSync === false` are SKIPPED — this is the same pass the engine runs,
   * not an override, so a manually-driven node stays manual.
   *
   * The engine already calls this on every scene graph each frame, right
   * after the physics step and before animations/tweens tick, so you rarely
   * need it by hand — reach for it after stepping physics yourself, or to
   * re-read transforms mid-frame before a raycast. With interpolation on
   * (Physics.setInterpolation) the transform read is the interpolated one.
   */
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

  /**
   * Node type as a string (read-only): 'group' | 'mesh' | 'skinnedMesh' |
   * 'instancedMesh' | 'light' | 'shape' | 'sprite' | 'physics' | 'html' |
   * 'gaussianSplat' | 'particles' | 'particles3d' | 'camera' | 'decal' |
   * 'reflectionProbe'. Use it to branch before touching a type-specific
   * property, since those silently no-op on the wrong type.
   */
  get type() {}

  /**
   * Light subtype (read-only): 'directional' | 'point' | 'spot'. undefined on
   * every non-light node. See docs/lighting-api.js.
   */
  get kind() {}

  /** Whether this node is visible. */
  get visible() {}
  set visible(value) {}

  /**
   * Optional camera-distance window (Godot visibility_range analog), on ANY
   * node type: `{begin, end, margin}` or null (default) to disable. The node
   * renders while `begin <= d < end`, where d is the world-space distance
   * from the camera eye to the node's world origin, evaluated once per
   * frame. `margin` (default 0 = hard switch) adds hysteresis: once shown
   * the node stays shown until d leaves [begin - margin, end + margin), once
   * hidden it stays hidden until d enters [begin + margin, end - margin) —
   * no popping when the camera hovers on a boundary.
   *
   * The gate is independent of `visible` and never writes it — both must
   * pass for the node to render. Like `visible`, a closed gate prunes the
   * subtree. Render-time only: scene.raycast() ignores it. A gated-out
   * light stops contributing; a gated-out shadow caster stops casting.
   *
   * @example
   *   // Swap a detailed prop for an imposter beyond 40 units:
   *   detail.visibilityRange = { begin: 0, end: 40, margin: 2 };
   *   imposter.visibilityRange = { begin: 40, end: 1e30, margin: 2 };
   */
  get visibilityRange() {}
  set visibilityRange(value) {}

  /** Parent SceneNode, or null if this is the root or a detached node. */
  get parent() {}

  /** Read-only array of child SceneNodes (snapshot; mutate via add()/remove()). */
  get children() {}

  /** Number of direct children. */
  get childCount() {}


  // --- Transform (all node types) -------------------------------------------

  /**
   * Whole-node position as [x, y, z]. Reads back a fresh array; assign a
   * 3-element array to set all axes in one write. Equivalent to setting
   * x/y/z individually.
   * @type {number[]}
   */
  get position() {}
  set position(value) {}

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

  /**
   * Orientation as an [x, y, z, w] quaternion. Unlike rotationX/Y/Z — which
   * round-trip through Euler angles on every set — this writes the node
   * orientation atomically, so it's the correct channel for arbitrary
   * rotations (6DOF cameras, port-to-port mating, physics-derived poses).
   * @type {number[]}
   */
  get quaternion() {}
  set quaternion(value) {}

  /**
   * Whole-node scale. Reads back as [x, y, z]; assign a uniform number or a
   * per-axis array (a short array leaves the remaining axes alone). Same
   * semantics as createMesh's `scale` option, and always consistent with
   * scaleX/scaleY/scaleZ below.
   * @type {number[]|number}
   */
  get scale() {}
  set scale(value) {}

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
   *     or setShaderUniform(). Numeric: float / vec2 / vec3 / vec4.
   *   - declare `uniform sampler2D u_*;` and feed it a single-channel float
   *     texture with setShaderTexture() — see below (static MeshNode only).
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
   * Bind a single-channel float texture to a `uniform sampler2D u_*`
   * declared by this node's custom shader. Static MeshNode only (not
   * instanced); requires a shader installed via setShader.
   *
   * Format contract: the data uploads as **R32F** (`GL_R32F` / `GL_RED` /
   * `GL_FLOAT`) with **LINEAR** magnification and **CLAMP_TO_EDGE** wrap on
   * both axes. Sampling in GLSL gives a bilinearly interpolated float in
   * `.r` — which is what a heightfield raymarcher wants: smooth between
   * texels, no wrap-around at the borders. `data` must hold exactly
   * width*height floats; a short array throws.
   *
   * Upload is staged, not immediate: the bytes are copied on the calling
   * thread and uploaded on the GL thread at the start of the next frame
   * that draws this node. The caller's Float32Array may be reused or
   * dropped as soon as the call returns.
   *
   * **Mipmaps** (`mipmap: true`, off by default): generates the full chain
   * and switches minification to `LINEAR_MIPMAP_LINEAR`. Needed whenever the
   * shader calls `textureLod(u_tex, uv, lod)` with a FRACTIONAL lod and
   * expects GL to blend the two bracketing levels — without a chain there is
   * only level 0, and every lod > 0 reads as 0. Also what you want for a
   * texture minified in screen space (a heightfield on a distant mesh), at
   * the cost of ~33% more texture memory and a generate pass per upload.
   * The flag is per slot and sticks until the next full upload changes it.
   *
   * **Sub-rectangle updates** (`x` / `y`): passing either key updates just
   * that region of the existing texture via `glTexSubImage2D` instead of
   * reallocating — `width`/`height` then describe the RECT, and `data` holds
   * the rect's width*height floats row-major. The mip chain is preserved
   * (regenerated after the write when the slot is mipmapped). Use this for
   * streaming edits — a terrain brush, a repainted tile — where a full
   * re-upload of a large texture per frame is the actual cost.
   *
   * Unlike the full-upload path, a rejected sub-update does not throw: it
   * logs a warning and is ignored. Rejected when the slot does not exist,
   * has never been given dimensions, or the rect falls outside them —
   * never a partial or out-of-bounds write. Queue order is preserved, and a
   * full upload supersedes any sub-updates staged before it.
   *
   * Texture-unit budget: the material uber-shader owns units 0-9 (baseColor
   * 0, shadow atlas 1, IBL irradiance/prefilter/BRDF 2/3/4, normal 5,
   * metallic-roughness 6, AO 7, emissive 8, reflection probe 9). User
   * samplers start at unit **10**, and GL 3.3 guarantees only 16 combined
   * texture image units — so a node may bind at most **6** sampler
   * uniforms. Asking for a 7th throws rather than reusing a material unit,
   * where the collision would silently corrupt material sampling instead of
   * erroring. Releasing a slot frees its unit for a later name.
   *
   * Passing null/undefined releases the slot (the GL texture is deleted on
   * the GL thread). Setting a name the shader never declares is silently
   * ignored, mirroring GL and setShaderUniform.
   *
   *   // raymarch a terrain heightfield in a sky-dome fragment shader
   *   const N = 256;
   *   const height = new Float32Array(N * N);
   *   for (let y = 0; y < N; y++)
   *     for (let x = 0; x < N; x++) height[y * N + x] = terrainAt(x, y);
   *   dome.setShader({
   *     fragment: `
   *       uniform sampler2D u_height;
   *       uniform vec2 u_extent;
   *       void userFragment(inout vec3 baseColor, inout vec3 normal,
   *                         inout float metallic, inout float roughness,
   *                         inout vec3 emissive, inout float alpha) {
   *         float h = texture(u_height, vWorldPos.xz / u_extent + 0.5).r;
   *         emissive += vec3(h);
   *       }`,
   *     uniforms: { u_extent: [1024, 1024] },
   *   });
   *   dome.setShaderTexture('u_height', { width: N, height: N, data: height });
   *
   *   // repaint one 16x16 tile without touching the rest
   *   dome.setShaderTexture('u_height',
   *     { x: 32, y: 48, width: 16, height: 16, data: tile });
   *
   *   dome.setShaderTexture('u_height', null);   // release
   *
   * The shadow pass binds these samplers exactly as the color pass does, so
   * a vertex chunk that displaces geometry by sampling one casts the
   * matching displaced silhouette.
   *
   * @param {string} name - must use the `u_` prefix
   * @param {?{width: number, height: number, data: Float32Array,
   *           mipmap?: boolean, x?: number, y?: number}} tex -
   *        width*height floats, or null to release the slot. `mipmap`
   *        generates a mip chain; `x`/`y` make it a sub-rectangle update
   *        (then width/height describe the rect).
   * @returns {SceneNode} this
   * @throws {TypeError} non-`u_` name, non-mesh node, no shader installed,
   *         non-positive extent, data shorter than width*height, or more
   *         than 6 sampler slots on one node. Sub-rect rejections warn and
   *         are ignored rather than throwing.
   */
  setShaderTexture(name, tex) {}

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
  // Model: one BASE track — a single clip or a registered BLEND SPACE
  // (addBlendSpace1D/2D + setBlendPos; Godot BlendSpace analog with shared-
  // phase sync); play() crossfades from whatever was playing — plus up to 8
  // ordered masked LAYER tracks blended on top via playLayer(slot, ...)
  // (e.g. upper-body wave over a walk; play() with a mask is layer slot 0).
  // blendState() reports the live mix. Full blending semantics + locomotion
  // recipe: docs/animation-api.js, "Skeletal blending".
  //
  // Above that sits an optional STATE MACHINE (addStateMachine + travel), the
  // Godot AnimationTree/StateMachine analog: named states each backed by a
  // clip or blend space, with declared transitions carrying their own fade
  // time. travel() names the destination and the machine picks the transition;
  // `node.state` reads the current one and `onStateChanged` fires after every
  // switch. There is no condition language — gameplay code decides when to
  // travel. A manual play()/stop() suspends the machine (state reads null)
  // until the next travel(). Full semantics: docs/animation-api.js,
  // "State machine".
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
   * Start a clip or blend space (see also play/stop/pause/resume above).
   *
   * Without `mask`, the name takes the BASE track — a clip from addClip or
   * a blend space from addBlendSpace1D/2D (a space shadows a same-named
   * clip; spaces always loop). With fadeTime > 0 the player crossfades
   * from the current blended pose over that many seconds (the outgoing
   * track keeps advancing while it fades — blend spaces included). With
   * `mask`, this is shorthand for playLayer(0, name, opts).
   *
   * @param {string} name - a registered clip or blend space (throws otherwise)
   * @param {Object} [opts]
   * @param {boolean} [opts.loop=true] - false: hold the last frame and fire
   *        onAnimationFinished once (ignored by blend spaces)
   * @param {number} [opts.speed=1] - playback rate (negative plays backward)
   * @param {number} [opts.fadeTime=0] - crossfade seconds (base track only)
   * @param {number} [opts.weight=1] - blend weight; base: vs bind pose,
   *        layer: vs what's underneath
   * @param {Uint8Array|number[]} [opts.mask] - per-bone 0/1, length =
   *        skeleton bone count; non-empty routes to layer slot 0
   * @returns {SceneNode} this
   */
  // play(name, opts) — documented with the shared play() above.

  /**
   * Register a 1D blend space: clips at scalar parameter positions
   * (e.g. speed). play(name) makes it the base track; setBlendPos picks
   * the mix (clamped to [min, max]; the two neighbors blend by position).
   * All member clips advance on one shared normalized phase so gait
   * cycles stay foot-aligned. Replaces a same-named space in place.
   * Full semantics: docs/animation-api.js, "Skeletal blending".
   * @param {string} name
   * @param {Array<{clip: string, pos: number, timescale?: number}>} points
   *        clip names must already be registered via addClip; timescale
   *        compensates cadence (2 = counts as a half-length cycle)
   * @returns {SceneNode} this
   */
  addBlendSpace1D(name, points) {}

  /**
   * Register a 2D blend space (e.g. strafe x/z velocity). The 3 nearest
   * points blend by normalized inverse-squared-distance weights — on a
   * sample point that clip takes full weight; degenerate layouts
   * (coincident/collinear points) are safe. Simpler than Godot's
   * triangulated BlendSpace2D; see docs/animation-api.js for the
   * trade-off. Same phase sync as 1D.
   * @param {string} name
   * @param {Array<{clip: string, pos: [number, number], timescale?: number}>} points
   * @returns {SceneNode} this
   */
  addBlendSpace2D(name, points) {}

  /**
   * Move a blend space's parameter — instant, no internal smoothing
   * (tween it from app code for easing); re-poses immediately even while
   * paused. 1D clamps x to the space's range; y is ignored. Also accepts
   * setBlendPos(name, [x, y]).
   * @param {string} name - a registered blend space (throws otherwise)
   * @param {number|number[]} x
   * @param {number} [y]
   * @returns {SceneNode} this
   */
  setBlendPos(name, x, y) {}

  /**
   * Start a clip on layer `slot` (0..7), replacing that slot atomically.
   * Layers blend over the base in ascending slot order; each is
   * independently masked, weighted, and fadeable. Blend spaces are
   * base-track only. Accepts the same opts as play(), where `fadeTime`
   * fades the layer's WEIGHT in from 0 and `mask` empty/omitted means the
   * whole body. A non-looping layer expires on finish (fires
   * onAnimationFinished); looping layers persist.
   * @param {number} slot
   * @param {string} name - a clip registered with addClip
   * @param {Object} [opts] - {loop, speed, fadeTime, weight, mask}
   * @returns {SceneNode} this
   */
  playLayer(slot, name, opts) {}

  /**
   * Fade layer `slot` out over opts.fadeTime seconds (0/omitted =
   * immediately) and free the slot.
   * @param {number} slot
   * @param {{fadeTime?: number}} [opts]
   * @returns {SceneNode} this
   */
  stopLayer(slot, opts) {}

  /**
   * Set a layer's blend weight at runtime (instant; multiplied by any
   * in-progress fade). Throws on an empty slot.
   * @param {number} slot
   * @param {number} weight
   * @returns {SceneNode} this
   */
  setLayerWeight(slot, weight) {}

  /**
   * Install a state machine on the player and enter its initial state
   * (defaults to the first state). Replaces any existing machine. Every
   * state's `source` must already be registered via addClip or
   * addBlendSpace1D/2D — otherwise this throws.
   *
   * @param {Object} def
   * @param {Array<{name: string, source: string, speed?: number,
   *                loop?: boolean}>} def.states
   *        `source` names a registered clip or blend space; speed defaults
   *        to 1, loop to true.
   * @param {Array<{from: string, to: string, fade?: number,
   *                autoAdvance?: boolean, syncPhase?: boolean}>} def.transitions
   *        `from` may be '*' as a wildcard fallback. `fade` is the crossfade
   *        in seconds (default 0 = hard cut). `autoAdvance` fires the
   *        transition automatically when a non-looping source finishes.
   *        `syncPhase` carries the outgoing normalized phase into the
   *        incoming source — keeps gait cycles foot-aligned across a switch.
   * @param {string} [def.initial] - starting state (default: states[0])
   * @returns {SceneNode} this
   */
  addStateMachine(def) {}

  /**
   * Switch to `stateName`, following the transition declared from the
   * current state (falling back to a '*' wildcard). With no matching
   * transition it warns and hard-switches (fade 0). Traveling to the
   * current state is a no-op. Throws on an unknown state, or if no machine
   * has been installed.
   * @param {string} stateName
   * @returns {SceneNode} this
   */
  travel(stateName) {}

  /**
   * Current state machine state name (read-only), or null when there is no
   * machine or the machine is suspended by a manual play()/stop().
   * @type {?string}
   */
  get state() {}

  /**
   * Called after every machine transition (travel or autoAdvance) with
   * (fromState, toState). `fromState` is null when re-entering from the
   * suspended state. Assign null to clear.
   * @type {?function(?string, string): void}
   */
  set onStateChanged(fn) {}

  /**
   * Snapshot of the current blend mix — cheap enough for HUDs/tests.
   * @returns {{state: ?string,
   *            clips: Array<{name: string, weight: number}>,
   *            phase: number, pos?: number[],
   *            layers: Array<{slot: number, name: string,
   *                           weight: number, phase: number}>}}
   *          state = current state machine state, always present but null
   *          without a machine (or while one is suspended);
   *          clips = base-track composition (weights sum to 1; during a
   *          crossfade the outgoing source appears scaled by 1 - alpha);
   *          phase = blend space shared phase 0..1 (clip: time/duration);
   *          pos present while a blend space is the base track.
   */
  blendState() {}

  /** Playback rate multiplier of the base track (get/set). */
  get animationSpeed() {}
  set animationSpeed(value) {}

  /**
   * Base-track clock in seconds (get/set). Setting scrubs: the pose,
   * palette, and getBoneWorldMatrix update immediately, even while paused.
   * Wraps into [0, duration) for looping clips, clamps for one-shots.
   * For a blend space this is phase × the blended cycle duration; setting
   * scrubs the shared phase.
   */
  get animationTime() {}
  set animationTime(value) {}

  /**
   * Duration in seconds of the current base clip (0 when none); for a
   * blend space, the current weight-mixed cycle duration.
   */
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

  /**
   * Emissive tint as [r,g,b] linear (MeshNode only). The shader emits
   * baseColor + emissiveColor*emissive, so retinting a glowing node means
   * setting this as well as `color` — changing `color` alone leaves the glow
   * its original hue. Assign a CSS string or an [r,g,b] array.
   */
  get emissiveColor() {}
  set emissiveColor(rgbOrCssString) {}

  /**
   * Node colour. On a MeshNode (skinned included) this is the material albedo
   * — the same channel createMesh's `color` option writes — and reads back as
   * [r,g,b,a]. On a LightNode it is the light's [r,g,b] linear colour. Assign
   * a CSS string or an [r,g,b] / [r,g,b,a] array; a mesh keeps its existing
   * alpha unless the value supplies a fourth component. Returns undefined on
   * node types that have no colour.
   */
  get color() {}
  set color(rgbOrCssString) {}

  /**
   * Install a discrete LOD chain (plain MeshNode only). Each frame the
   * renderer picks the first level whose `maxDist` exceeds the camera
   * distance (measured to the node's world origin, once per frame); beyond
   * the last maxDist the coarsest level keeps drawing — combine with
   * `visibilityRange` to cull entirely. The shadow pass always draws the
   * SAME selected level as the color pass, so a caster can never shadow
   * with a different silhouette than it renders.
   *
   * A non-empty chain replaces the base mesh for RENDERING only: the base
   * mesh (createMesh's `mesh` / updateMesh) remains the raycast/picking
   * source, so set it to the highest-detail level when the node must stay
   * pickable. Culling bounds are the union of all levels, so switches never
   * pop. Pass an empty array to clear (back to the base mesh). Not
   * supported on SkinnedMeshNode or InstancedMeshNode (throws; per-instance
   * LOD is a separate feature).
   *
   * @param {Array<{mesh: Mesh, maxDist: number}>} levels
   * @returns {SceneNode} this
   * @example
   *   const node = scene.createMesh({ mesh: hiMesh, color: 'gray' });
   *   node.setLodMeshes([
   *     { mesh: hiMesh,  maxDist: 20 },
   *     { mesh: midMesh, maxDist: 60 },
   *     { mesh: loMesh,  maxDist: 1e30 },
   *   ]);
   */
  setLodMeshes(levels) {}

  /** Selected LOD chain index from the most recent rendered frame (MeshNode
   *  with a chain; undefined otherwise). Read-only. */
  get lodLevel() {}

  /** Number of LOD chain levels (MeshNode only; 0 = no chain). Read-only. */
  get lodCount() {}


  // --- InstancedMeshNode-only ------------------------------------------------
  //
  // Runtime surface for nodes made by scene.createInstancedMesh (see there for
  // the instance-buffer layout). Every method below is a silent no-op on any
  // other node type, and the properties read back undefined.

  /**
   * Replace the whole instance buffer. `data` is a Float32Array of 16 floats
   * per instance in the canonical layout; the count is derived from its
   * length. Uploads once — cheap enough to call per frame for a few thousand
   * instances, but prefer updateInstance() when only a handful moved.
   * @param {Float32Array} data
   */
  setInstances(data) {}

  /**
   * Replace the whole instance buffer from the 9-floats-per-instance
   * convenience form: (px, py, pz, qx, qy, qz, qw, scale, variantIndex).
   * RGB defaults to white; variantIndex is packed into alpha.
   * @param {Float32Array} data
   */
  setInstancesFromTransforms(data) {}

  /**
   * Rewrite ONE instance's 16-float record in place — the cheap path for a
   * few moving instances in a large static batch. `data16` must hold at
   * least 16 floats; out-of-range indices are ignored.
   * @param {number} index
   * @param {Float32Array} data16
   */
  updateInstance(index, data16) {}

  /**
   * Swap the replicated mesh, keeping the instance buffer and material.
   * @param {Mesh} mesh
   */
  setInstancedMesh(mesh) {}

  /**
   * Split the base-color texture into a cols x rows grid of variants. With a
   * grid set, each instance's alpha channel selects its cell (0..255 → cell
   * index), so one batch can show several sprites/leaf shapes from a shared
   * atlas. Pass (1, 1) to disable.
   * @param {number} cols
   * @param {number} rows
   */
  setAtlasGrid(cols, rows) {}

  /** Number of instances currently in the buffer (read-only). */
  get instanceCount() {}

  /** Atlas grid columns (read-only; 0 on non-instanced nodes). */
  get atlasCols() {}

  /** Atlas grid rows (read-only; 0 on non-instanced nodes). */
  get atlasRows() {}

  /**
   * Alpha-test cutoff. > 0 discards fragments below the threshold (cutout
   * leaf cards); 0 disables. Also settable via setAlphaCutoff(c).
   */
  get alphaCutoff() {}
  set alphaCutoff(value) {}

  /**
   * Disable backface culling for the batch — needed so the back face of a
   * double-sided leaf card renders. Also settable via setDoubleSided(b).
   */
  get doubleSided() {}
  set doubleSided(value) {}

  /** Method form of the `alphaCutoff` property. @param {number} c */
  setAlphaCutoff(c) {}

  /** Method form of the `doubleSided` property. @param {boolean} b */
  setDoubleSided(b) {}


  // --- LightNode-only -------------------------------------------------------

  /** [x,y,z] direction vector (directional/spot lights). */
  get direction() {}
  set direction(xyz) {}

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
   * Orient this node so its local -Z axis points at a world-space target,
   * keeping local +Y as close to world up as possible. The camera-node
   * aiming convention (a camera looks down -Z); also handy for turrets and
   * spot rigs. Writes the LOCAL rotation, compensating for ancestor
   * rotations (exact for TRS hierarchies; ancestor non-uniform scale is
   * ignored).
   * @param {number|number[]} x - target X, or a full [x, y, z] array
   * @param {number} [y]
   * @param {number} [z]
   * @returns {SceneNode} this
   */
  lookAt(x, y, z) {}

  /**
   * Transform a local-space point to world space.
   * @param {number} x
   * @param {number} y
   * @param {number} [z=0]
   * @returns {{ x: number, y: number, z: number }}
   */
  localToWorld(x, y, z) {}


  // --- Audio emitter (all node types) ---------------------------------------

  /**
   * Attach a broaudio playback (or voice) handle to this node — the
   * AudioStreamPlayer3D analog. Each engine frame (after tweens/animations,
   * before audio renders) the engine pushes the node's WORLD position into
   * the source's spatial position and derives its velocity from the previous
   * frame's position over the scaled dt, feeding the Doppler model
   * (audio-api.js) — no per-frame JS.
   *
   * Attaching enables spatialization on the handle
   * (setPlaybackSpatialEnabled / setVoiceSpatialEnabled) and pushes the
   * current position immediately. Tune rolloff/refDistance/maxDistance via
   * the normal ctx.setPlaybackSpatial* / setVoiceSpatial* calls. One emitter
   * per node (re-attach replaces); any number of emitting nodes per scene.
   * A handle that finishes or is stopped simply makes the sync a no-op —
   * attach a fresh handle to reuse the node. The binding dies with the node.
   * Note: streaming playbacks position/attenuate but do not Doppler-shift
   * (see audio-api.js).
   *
   * @param {number} handle - playbackId (default) or voiceId (opts.voice)
   * @param {{voice?: boolean}} [opts] - pass { voice: true } for a synth voice id
   * @example
   *   const engineSound = ctx.playClip(engineClip, 1.0, true);
   *   carNode.attachAudioEmitter(engineSound);
   *   scene.bindAudioListenerToCamera(true);
   *   // driving the car node around now pans/attenuates/Dopplers the loop
   */
  attachAudioEmitter(handle, opts) {}

  /** Remove this node's audio emitter binding (the audio keeps playing,
   *  position just stops following the node). */
  detachAudioEmitter() {}


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
