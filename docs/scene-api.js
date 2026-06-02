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
   * Create a 2D tilemap node. Stores N layers of `columns x rows` tile
   * indices and renders each layer in order via batched drawImage calls.
   * Tile value 0 = empty (skipped). Tile values 1..N index the tileset
   * grid (row-major, 1-based).
   *
   * @example
   *   // Single dense layer
   *   const map = scene.createTilemap({
   *     tileWidth: 32, tileHeight: 32,
   *     columns: 40, rows: 30,
   *     tileset: { src: 'assets/tiles.png', tileWidth: 32, tileHeight: 32, columns: 8 },
   *     data: new Uint16Array(40 * 30)   // 0 = empty
   *   });
   *   map.setTile(5, 10, 7);                 // place tile 7 at (col=5,row=10)
   *   map.getTile(5, 10);                    // -> 7
   *   const hit = map.tileAtWorld(160, 320); // -> { col, row } | null
   *
   *   // Multiple named layers (rendered in order)
   *   scene.createTilemap({
   *     tileWidth: 16, tileHeight: 16, columns: 64, rows: 64,
   *     tileset: { src: 'assets/tiles.png', tileWidth: 16, tileHeight: 16, columns: 16 },
   *     layers: [
   *       { name: 'ground', data: groundData },
   *       { name: 'decals', data: decalData }
   *     ]
   *   });
   *
   * @param {Object} opts
   * @param {string} [opts.name]
   * @param {number} opts.tileWidth - per-tile output width (node-local pixels)
   * @param {number} opts.tileHeight
   * @param {number} opts.columns
   * @param {number} opts.rows
   * @param {number} [opts.x]
   * @param {number} [opts.y]
   * @param {Object} opts.tileset
   * @param {string} opts.tileset.src - tileset image path
   * @param {number} [opts.tileset.tileWidth] - per-cell width in image pixels
   * @param {number} [opts.tileset.tileHeight]
   * @param {number} [opts.tileset.columns] - 0 = auto from image width
   * @param {Uint16Array|number[]} [opts.data] - single-layer dense data
   * @param {Array<{name:string,data:(Uint16Array|number[])}>} [opts.layers] - multi-layer (overrides `data`)
   * @returns {SceneNode}
   */
  createTilemap(opts) {}

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
   * @param {number} [opts.scale=1] - uniform scale
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
   * @returns {SceneNode}
   */
  createMesh(opts) {}

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
   * Set exponential distance fog. Fragments beyond `end` are fully fogged to
   * `color`; closer than `start` are unaffected. Set start=end=0 to disable.
   *
   * @param {Object} opts
   * @param {number} [opts.start=0] - fog start distance (world units)
   * @param {number} [opts.end=0]   - fog end distance (world units)
   * @param {number[]} [opts.color=[0,0,0]] - [r,g,b] in 0-1
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
   * @param {string} [name]
   * @returns {SceneNode} this
   */
  play(name) {}

  /**
   * SpriteNode: pause animation playback (current frame is held).
   * ParticleNode: stop emitting; existing particles finish naturally.
   * @returns {SceneNode} this
   */
  stop() {}

  /**
   * SpriteNode only: register or replace a named animation at runtime.
   * The spec is the same shape as the createSprite `animations` entry.
   * @param {string} name
   * @param {{frames:number[], fps?:number, loop?:boolean, next?:string}} spec
   */
  addAnimation(name, spec) {}


  // --- ParticleNode ---------------------------------------------------------

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
   * createParticles (rate, lifetime, velocity, gravity, size, color,
   * rotation, drag, blend, texture, maxParticles).
   * @param {Object} opts
   */
  configure(opts) {}


  // --- TilemapNode ----------------------------------------------------------

  /**
   * Set a tile by (col, row). 0 = empty. Out-of-bounds is silently
   * ignored. `layer` may be an index or a string layer name; defaults
   * to 0.
   */
  setTile(col, row, tileIndex, layer) {}

  /** Returns the tile index at (col, row) — 0 if empty or out-of-bounds. */
  getTile(col, row, layer) {}

  /**
   * Map world coordinates to grid cell. Honors the tilemap's transform.
   * @returns {{col:number,row:number}|null}
   */
  tileAtWorld(worldX, worldY) {}

  /**
   * Replace the mesh on a MeshNode in place (keeps transform, color, etc).
   * Accepts the same `data`/`mesh`/`positions+indices` forms as createMesh.
   * No-op on non-mesh nodes.
   * @param {Object|Mesh} meshOrOpts
   */
  updateMesh(meshOrOpts) {}


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
