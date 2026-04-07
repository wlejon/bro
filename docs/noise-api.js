// =============================================================================
// bro FastNoise2 API Reference
// =============================================================================
//
// SIMD-accelerated procedural noise generation powered by FastNoise2.
// Available to all bro JS workers via the global `FastNoise` constructor.
//
// Two ways to create nodes:
//   const simplex = FastNoise.create("Simplex");   // generic (any type)
//   const simplex = FastNoise.Simplex();            // shortcut (common types)
//
// Configure with the generic set() method:
//   node.set("PropertyName", value);     // float, int, or enum string
//   node.set("Source", otherNode);        // node connection
//   node.set("Gain", 0.5);               // hybrid as constant
//   node.set("Gain", noiseNode);          // hybrid driven by noise
//
// Introspect any node:
//   node.getMembers();                    // { type, variables, nodes, hybrids }
//   FastNoise.types();                    // list all registered node types
//
// =============================================================================


// -----------------------------------------------------------------------------
// FastNoise (global constructor / namespace)
// -----------------------------------------------------------------------------

class FastNoise {

  // --- Construction ---------------------------------------------------------

  /**
   * Create from an encoded node tree string (from FN2 Node Editor).
   * @param {string} encodedNodeTree - Base64-encoded node graph
   */
  constructor(encodedNodeTree) {}

  /**
   * Create any node type by name. Works for all ~50 registered types.
   * @param {string} typeName - e.g. "Simplex", "FractalFBm", "Add"
   * @returns {FastNoise}
   */
  static create(typeName) {}

  /**
   * List all registered node types with their group categories.
   * @returns {Array<{name: string, groups: string[]}>}
   */
  static types() {}


  // --- Convenience Factories (common types) ---------------------------------

  static Simplex() {}          // Coherent Noise
  static SuperSimplex() {}     // Coherent Noise
  static Perlin() {}           // Coherent Noise
  static Value() {}            // Coherent Noise
  static CellularValue() {}    // Coherent Noise
  static CellularDistance() {}  // Coherent Noise
  static CellularLookup() {}   // Coherent Noise
  static FractalFBm() {}       // Fractal
  static FractalRidged() {}    // Fractal
  static DomainWarpGradient() {} // Domain Warp


  // --- Generic Configuration ------------------------------------------------

  /**
   * Set any property by name. Automatically routes to the correct member type
   * (variable, node lookup, or hybrid) using FN2 metadata.
   *
   * @param {string} name  - Property name (see per-type reference below)
   * @param {number|string|FastNoise} value
   *   - number: sets float/int variable, or hybrid constant
   *   - string: sets enum variable by name (e.g. "Manhattan")
   *   - FastNoise: connects a node source, or sets hybrid to node-driven
   */
  set(name, value) {}

  /**
   * Introspect this node's configurable members.
   * @returns {{
   *   type: string,
   *   variables: Array<{name: string, type: "float"|"int"|"enum", enumValues?: string[]}>,
   *   nodes: Array<{name: string}>,
   *   hybrids: Array<{name: string, default: number}>
   * }}
   */
  getMembers() {}


  // --- Generation -----------------------------------------------------------

  /**
   * Sample a single 2D noise value.
   * @param {number} x - X coordinate
   * @param {number} y - Y coordinate
   * @param {number} seed - Integer seed
   * @returns {number}
   */
  genSingle2D(x, y, seed) {}

  /**
   * Sample a single 3D noise value.
   * @param {number} x
   * @param {number} y
   * @param {number} z
   * @param {number} seed
   * @returns {number}
   */
  genSingle3D(x, y, z, seed) {}

  /**
   * Generate a 2D grid of noise values. Output layout: out[y * xSize + x]
   * @param {number} xOffset - Starting X position in world space
   * @param {number} yOffset - Starting Y position in world space
   * @param {number} xSize   - Number of samples along X
   * @param {number} ySize   - Number of samples along Y
   * @param {number} frequency - Step size between samples (both axes)
   * @param {number} seed
   * @returns {Float32Array}
   */
  genUniformGrid2D(xOffset, yOffset, xSize, ySize, frequency, seed) {}

  /**
   * Generate a 3D grid of noise values. Output: out[(z * ySize + y) * xSize + x]
   * @param {number} xOffset
   * @param {number} yOffset
   * @param {number} zOffset
   * @param {number} xSize
   * @param {number} ySize
   * @param {number} zSize
   * @param {number} frequency - Step size between samples (all axes)
   * @param {number} seed
   * @returns {Float32Array}
   */
  genUniformGrid3D(xOffset, yOffset, zOffset, xSize, ySize, zSize, frequency, seed) {}

  /**
   * Generate seamlessly tileable 2D noise. Maps onto a 4D hypertorus internally.
   * @param {number} xSize - Tile width in samples
   * @param {number} ySize - Tile height in samples
   * @param {number} frequency - Step size between samples
   * @param {number} seed
   * @returns {Float32Array}
   */
  genTileable2D(xSize, ySize, frequency, seed) {}
}


// =============================================================================
// NODE TYPE REFERENCE
// =============================================================================
//
// Every node type and its configurable properties. Properties are set via
// node.set("Name", value). Types are:
//
//   VAR (float/int)    — set with a number
//   VAR (enum)         — set with a string name or int index
//   NODE               — set with another FastNoise instance
//   HYBRID             — set with a number (constant) or FastNoise (node-driven)
//
// Per-dimension properties (marked with [per-dim]) appear multiple times in
// getMembers() — one per dimension (X, Y, Z, W). They share the same base
// name; the dimension index distinguishes them internally.
//
// =============================================================================


// =============================================================================
// BASIC GENERATORS
// =============================================================================

// --- Constant ---------------------------------------------------------------
// Outputs a fixed value everywhere. Useful as input to blend/operator nodes.
//
//   VAR  "Value"          (float)   default: 1.0
//
//   const c = FastNoise.create("Constant");
//   c.set("Value", 0.5);

// --- White ------------------------------------------------------------------
// Pure white noise (random value per cell).
//
//   VAR  "Seed Offset"    (int)
//   VAR  "Output Min"     (float)
//   VAR  "Output Max"     (float)

// --- Checkerboard -----------------------------------------------------------
// Alternating +1/-1 checkerboard pattern.
//
//   VAR  "Feature Scale"  (float)
//   VAR  "Output Min"     (float)
//   VAR  "Output Max"     (float)

// --- SineWave ---------------------------------------------------------------
// Smooth sine wave pattern.
//
//   VAR  "Feature Scale"  (float)
//   VAR  "Output Min"     (float)
//   VAR  "Output Max"     (float)

// --- Gradient ---------------------------------------------------------------
// Linear gradient along axes.
//
//   VAR     "Multiplier"  (float)   [per-dim]
//   HYBRID  "Offset"      (float)   [per-dim]  default: 0

// --- DistanceToPoint --------------------------------------------------------
// Distance from each point to a configurable target.
//
//   VAR     "Distance Function" (enum) {Euclidean, Euclidean Squared, Manhattan,
//                                        Hybrid, Max Axis, Minkowski}
//   HYBRID  "Point"       (float)   [per-dim]  default: 0
//   HYBRID  "Minkowski P" (float)   default: 1.5


// =============================================================================
// COHERENT NOISE
// =============================================================================

// --- Simplex ----------------------------------------------------------------
//   VAR  "Feature Scale"  (float)
//   VAR  "Seed Offset"    (int)
//   VAR  "Output Min"     (float)
//   VAR  "Output Max"     (float)
//
//   const n = FastNoise.create("Simplex");
//   n.set("Feature Scale", 0.02);

// --- SuperSimplex -----------------------------------------------------------
//   (same properties as Simplex)

// --- Perlin -----------------------------------------------------------------
//   (same properties as Simplex)

// --- Value ------------------------------------------------------------------
//   (same properties as Simplex)


// =============================================================================
// CELLULAR NOISE
// =============================================================================

// --- CellularValue ----------------------------------------------------------
// Returns the noise value of the closest cell.
//
//   VAR     "Feature Scale"      (float)
//   VAR     "Seed Offset"        (int)
//   VAR     "Output Min"         (float)
//   VAR     "Output Max"         (float)
//   VAR     "Distance Function"  (enum)  {Euclidean, Euclidean Squared, Manhattan,
//                                          Hybrid, Max Axis, Minkowski}
//   VAR     "Value Index"        (int)   0-3, which closest cell to use
//   HYBRID  "Minkowski P"        (float) default: 1.5
//   HYBRID  "Grid Jitter"        (float) default: 1.0
//   HYBRID  "Size Jitter"        (float) default: 0.0
//
//   const cv = FastNoise.create("CellularValue");
//   cv.set("Distance Function", "Manhattan");
//   cv.set("Grid Jitter", 0.8);

// --- CellularDistance -------------------------------------------------------
// Returns the distance to cell boundaries.
//
//   VAR     "Feature Scale"      (float)
//   VAR     "Seed Offset"        (int)
//   VAR     "Output Min"         (float)
//   VAR     "Output Max"         (float)
//   VAR     "Distance Function"  (enum)  {Euclidean, Euclidean Squared, Manhattan,
//                                          Hybrid, Max Axis, Minkowski}
//   VAR     "Distance Index 0"   (int)   0-3
//   VAR     "Distance Index 1"   (int)   0-3
//   VAR     "Return Type"        (enum)  {Index0, Index0Add1, Index0Sub1,
//                                          Index0Mul1, Index0Div1}
//   HYBRID  "Minkowski P"        (float) default: 1.5
//   HYBRID  "Grid Jitter"        (float) default: 1.0
//   HYBRID  "Size Jitter"        (float) default: 0.0
//
//   const cd = FastNoise.create("CellularDistance");
//   cd.set("Return Type", "Index0Sub1");
//   cd.set("Distance Function", "Euclidean");

// --- CellularLookup ---------------------------------------------------------
// Evaluates a lookup generator at the position of the closest cell.
//
//   VAR     "Feature Scale"      (float)
//   VAR     "Seed Offset"        (int)
//   VAR     "Distance Function"  (enum)  {Euclidean, Euclidean Squared, Manhattan,
//                                          Hybrid, Max Axis, Minkowski}
//   NODE    "Lookup"             — generator evaluated at cell positions
//   HYBRID  "Minkowski P"        (float) default: 1.5
//   HYBRID  "Grid Jitter"        (float) default: 1.0
//   HYBRID  "Size Jitter"        (float) default: 0.0
//
//   const cl = FastNoise.create("CellularLookup");
//   cl.set("Lookup", FastNoise.create("Simplex"));


// =============================================================================
// FRACTAL
// =============================================================================

// --- FractalFBm -------------------------------------------------------------
// Fractal Brownian Motion — stacks octaves of a source generator.
//
//   NODE    "Source"              — the base generator to fractalise
//   VAR     "Octaves"            (int)    default: 3
//   VAR     "Lacunarity"         (float)  default: 2.0
//   HYBRID  "Gain"               (float)  default: 0.5
//   HYBRID  "Weighted Strength"  (float)  default: 0.0
//
//   const fbm = FastNoise.create("FractalFBm");
//   fbm.set("Source", FastNoise.create("Simplex"));
//   fbm.set("Octaves", 5);
//   fbm.set("Gain", 0.5);
//   fbm.set("Lacunarity", 2.0);

// --- FractalRidged ----------------------------------------------------------
// Ridged multifractal — absolute-value inversion creates ridges.
//
//   NODE    "Source"              — the base generator
//   VAR     "Octaves"            (int)    default: 3
//   VAR     "Lacunarity"         (float)  default: 2.0
//   HYBRID  "Gain"               (float)  default: 0.5
//   HYBRID  "Weighted Strength"  (float)  default: 0.0
//
//   const ridged = FastNoise.create("FractalRidged");
//   ridged.set("Source", FastNoise.create("Perlin"));
//   ridged.set("Octaves", 4);


// =============================================================================
// DOMAIN WARP
// =============================================================================

// --- DomainWarpGradient -----------------------------------------------------
// Warps input coordinates using gradient-based displacement.
//
//   NODE    "Source"              — generator whose domain is warped
//   VAR     "Feature Scale"      (float)
//   VAR     "Seed Offset"        (int)
//   HYBRID  "Warp Amplitude"     (float)  default: 1.0
//
//   const warp = FastNoise.create("DomainWarpGradient");
//   warp.set("Source", FastNoise.create("Simplex"));
//   warp.set("Warp Amplitude", 50.0);

// --- DomainWarpSimplex ------------------------------------------------------
// Domain warp using simplex-style vectors.
//
//   NODE    "Source"
//   VAR     "Feature Scale"      (float)
//   VAR     "Seed Offset"        (int)
//   HYBRID  "Warp Amplitude"     (float)  default: 1.0

// --- DomainWarpSuperSimplex -------------------------------------------------
// Domain warp using super-simplex vectors.
//
//   (same properties as DomainWarpSimplex)


// =============================================================================
// DOMAIN WARP FRACTAL
// =============================================================================

// --- DomainWarpFractalProgressive -------------------------------------------
// Applies fractal octaves to a domain warp, warping progressively.
//
//   NODE    "Domain Warp Source"  — must be a DomainWarp node
//   VAR     "Octaves"            (int)    default: 3
//   VAR     "Lacunarity"         (float)  default: 2.0
//   HYBRID  "Gain"               (float)  default: 0.5
//   HYBRID  "Weighted Strength"  (float)  default: 0.0
//
//   const dwGrad = FastNoise.create("DomainWarpGradient");
//   dwGrad.set("Source", FastNoise.create("Simplex"));
//   const dwFrac = FastNoise.create("DomainWarpFractalProgressive");
//   dwFrac.set("Domain Warp Source", dwGrad);
//   dwFrac.set("Octaves", 4);

// --- DomainWarpFractalIndependent -------------------------------------------
// Applies fractal octaves to a domain warp, each octave independent.
//
//   (same properties as DomainWarpFractalProgressive)


// =============================================================================
// OPERATORS / BLENDS
// =============================================================================
//
// Operators combine two noise sources. LHS is always a node connection.
// RHS is a hybrid (accepts a constant float or another noise node).
// Some operators (Subtract, Divide, Modulus) have hybrid LHS too.

// --- Add --------------------------------------------------------------------
//   NODE    "LHS"    — left-hand source
//   HYBRID  "RHS"    (float) default: 0.0
//
//   const add = FastNoise.create("Add");
//   add.set("LHS", terrainBase);
//   add.set("RHS", 0.1);             // constant offset
//   add.set("RHS", anotherNoise);    // or add two noise sources

// --- Subtract ---------------------------------------------------------------
//   HYBRID  "LHS"    (float) default: 0.0  — also accepts constant
//   HYBRID  "RHS"    (float) default: 0.0

// --- Multiply ---------------------------------------------------------------
//   NODE    "LHS"
//   HYBRID  "RHS"    (float) default: 1.0
//
//   const scaled = FastNoise.create("Multiply");
//   scaled.set("LHS", heightNoise);
//   scaled.set("RHS", 2.0);    // double the amplitude

// --- Divide -----------------------------------------------------------------
//   HYBRID  "LHS"    (float) default: 0.0
//   HYBRID  "RHS"    (float) default: 1.0

// --- Modulus ----------------------------------------------------------------
//   HYBRID  "LHS"    (float) default: 0.0
//   HYBRID  "RHS"    (float) default: 1.0

// --- Min --------------------------------------------------------------------
//   NODE    "LHS"
//   HYBRID  "RHS"    (float) default: 0.0
//
//   const clamped = FastNoise.create("Min");
//   clamped.set("LHS", heightNoise);
//   clamped.set("RHS", 0.8);   // cap at 0.8

// --- Max --------------------------------------------------------------------
//   NODE    "LHS"
//   HYBRID  "RHS"    (float) default: 0.0

// --- MinSmooth --------------------------------------------------------------
// Smooth minimum — blends near the boundary instead of a hard cutoff.
//
//   NODE    "LHS"
//   HYBRID  "RHS"        (float) default: 0.0
//   HYBRID  "Smoothness" (float) default: 0.1

// --- MaxSmooth --------------------------------------------------------------
//   NODE    "LHS"
//   HYBRID  "RHS"        (float) default: 0.0
//   HYBRID  "Smoothness" (float) default: 0.1

// --- Fade -------------------------------------------------------------------
// Interpolates between two sources based on a fade value.
//
//   NODE    "A"           — source when fade = min
//   NODE    "B"           — source when fade = max
//   HYBRID  "Fade"        (float) default: 0.5
//   HYBRID  "Fade Min"    (float) default: -1.0
//   HYBRID  "Fade Max"    (float) default: 1.0
//   VAR     "Interpolation" (enum) {Linear, Hermite, Quintic}
//
//   const blend = FastNoise.create("Fade");
//   blend.set("A", flatlands);
//   blend.set("B", mountains);
//   blend.set("Fade", moistureNoise);   // node-driven blending
//   blend.set("Interpolation", "Hermite");

// --- PowFloat ---------------------------------------------------------------
//   HYBRID  "Value"  (float) default: 2.0
//   HYBRID  "Pow"    (float) default: 2.0

// --- PowInt -----------------------------------------------------------------
//   NODE    "Value"
//   VAR     "Pow"    (int)   default: 2


// =============================================================================
// MODIFIERS
// =============================================================================

// --- Remap ------------------------------------------------------------------
// Remaps the output range of a source generator.
//
//   NODE    "Source"
//   HYBRID  "From Min"    (float) default: -1.0
//   HYBRID  "From Max"    (float) default: 1.0
//   HYBRID  "To Min"      (float) default: 0.0
//   HYBRID  "To Max"      (float) default: 1.0
//
//   const remap = FastNoise.create("Remap");
//   remap.set("Source", noise);
//   remap.set("From Min", -1.0);
//   remap.set("From Max", 1.0);
//   remap.set("To Min", 0.0);
//   remap.set("To Max", 1.0);

// --- Terrace ----------------------------------------------------------------
// Creates terrace/step-like output from continuous noise.
//
//   NODE    "Source"
//   VAR     "Step Count"   (float)
//   HYBRID  "Smoothness"   (float) default: 0.0
//
//   const terrace = FastNoise.create("Terrace");
//   terrace.set("Source", terrainNoise);
//   terrace.set("Step Count", 8.0);
//   terrace.set("Smoothness", 0.3);

// --- PingPong ---------------------------------------------------------------
// Folds the output into a ping-pong pattern.
//
//   NODE    "Source"
//   HYBRID  "Ping Pong Strength" (float) default: 2.0

// --- SeedOffset -------------------------------------------------------------
// Offsets the seed before passing to the source. Useful for creating
// independent variations from the same source graph.
//
//   NODE    "Source"
//   VAR     "Seed Offset"  (int)   default: 1

// --- GeneratorCache ---------------------------------------------------------
// Caches the source output. Useful when the same generator is used as input
// to multiple nodes to avoid redundant computation.
//
//   NODE    "Source"

// --- Abs --------------------------------------------------------------------
// Takes the absolute value of the source output.
//
//   NODE    "Source"

// --- SignedSquareRoot -------------------------------------------------------
// Applies sign-preserving square root.
//
//   NODE    "Source"

// --- ConvertRGBA8 -----------------------------------------------------------
// Converts noise to RGBA8 packed format.
//
//   NODE    "Source"
//   VAR     "Min"     (float)
//   VAR     "Max"     (float)


// =============================================================================
// DOMAIN MODIFIERS
// =============================================================================

// --- DomainScale ------------------------------------------------------------
// Scales the input coordinates uniformly.
//
//   NODE    "Source"
//   VAR     "Scaling"  (float) default: 1.0
//
//   const ds = FastNoise.create("DomainScale");
//   ds.set("Source", noise);
//   ds.set("Scaling", 0.5);   // halves feature frequency

// --- DomainOffset -----------------------------------------------------------
// Offsets input coordinates per-dimension.
//
//   NODE    "Source"
//   HYBRID  "Offset"  (float)  [per-dim] default: 0.0

// --- DomainRotate -----------------------------------------------------------
// Rotates input coordinates in 3D.
//
//   NODE    "Source"
//   VAR     "Yaw"    (float)   in degrees
//   VAR     "Pitch"  (float)   in degrees
//   VAR     "Roll"   (float)   in degrees

// --- DomainAxisScale --------------------------------------------------------
// Scales input coordinates per-axis independently.
//
//   NODE    "Source"
//   VAR     "Scaling"  (float)  [per-dim] default: 1.0

// --- AddDimension -----------------------------------------------------------
// Adds an extra dimension with a fixed or noise-driven position.
//
//   NODE    "Source"
//   HYBRID  "New Dimension Position"  (float)  default: 0.0

// --- RemoveDimension --------------------------------------------------------
// Removes a dimension from the input.
//
//   NODE    "Source"
//   VAR     "Remove Dimension"  (enum)  {X, Y, Z, W}


// =============================================================================
// EXAMPLES
// =============================================================================

// --- Terrain heightmap with FBm ---------------------------------------------

const terrainSrc = FastNoise.create("Simplex");
const terrain = FastNoise.create("FractalFBm");
terrain.set("Source", terrainSrc);
terrain.set("Octaves", 6);
terrain.set("Gain", 0.5);
terrain.set("Lacunarity", 2.0);

const heightmap = terrain.genUniformGrid2D(0, 0, 512, 512, 0.002, 1337);
// heightmap is a Float32Array with 262144 values


// --- Ridged mountains blended with plains -----------------------------------

const plains = FastNoise.create("FractalFBm");
plains.set("Source", FastNoise.create("Simplex"));
plains.set("Octaves", 4);

const mountains = FastNoise.create("FractalRidged");
mountains.set("Source", FastNoise.create("Perlin"));
mountains.set("Octaves", 5);

const biomeNoise = FastNoise.create("Simplex");

const blended = FastNoise.create("Fade");
blended.set("A", plains);
blended.set("B", mountains);
blended.set("Fade", biomeNoise);       // biome noise drives the blend
blended.set("Interpolation", "Hermite");

const world = blended.genUniformGrid2D(0, 0, 1024, 1024, 0.001, 42);


// --- 3D voxel density field for caves ---------------------------------------

const caveShape = FastNoise.create("FractalRidged");
caveShape.set("Source", FastNoise.create("Perlin"));
caveShape.set("Octaves", 3);
caveShape.set("Gain", 0.6);

const warped = FastNoise.create("DomainWarpGradient");
warped.set("Source", caveShape);
warped.set("Warp Amplitude", 30.0);

const density = warped.genUniformGrid3D(0, 0, 0, 64, 64, 64, 0.02, 7);
// density[z * 64*64 + y * 64 + x] — negative = solid, positive = air


// --- Domain warp fractal for organic terrain --------------------------------

const dwGrad = FastNoise.create("DomainWarpGradient");
dwGrad.set("Source", FastNoise.create("Simplex"));
dwGrad.set("Warp Amplitude", 20.0);

const dwFrac = FastNoise.create("DomainWarpFractalProgressive");
dwFrac.set("Domain Warp Source", dwGrad);
dwFrac.set("Octaves", 4);
dwFrac.set("Gain", 0.6);

const organic = dwFrac.genUniformGrid2D(0, 0, 256, 256, 0.005, 99);


// --- Cellular biome map -----------------------------------------------------

const biomes = FastNoise.create("CellularValue");
biomes.set("Distance Function", "Euclidean");
biomes.set("Grid Jitter", 0.8);
biomes.set("Feature Scale", 0.01);

const biomeMap = biomes.genUniformGrid2D(0, 0, 256, 256, 0.01, 55);


// --- Terraced plateaus ------------------------------------------------------

const baseTerrain = FastNoise.create("FractalFBm");
baseTerrain.set("Source", FastNoise.create("Simplex"));
baseTerrain.set("Octaves", 5);

const plateaus = FastNoise.create("Terrace");
plateaus.set("Source", baseTerrain);
plateaus.set("Step Count", 6.0);
plateaus.set("Smoothness", 0.2);

const terraced = plateaus.genUniformGrid2D(0, 0, 256, 256, 0.005, 123);


// --- Runtime introspection --------------------------------------------------

// List all available types
const allTypes = FastNoise.types();
// [{ name: "Constant", groups: ["Basic Generators"] },
//  { name: "White",    groups: ["Basic Generators"] },
//  { name: "Simplex",  groups: ["Coherent Noise"] },
//  ...50+ entries ]

// Inspect what a node accepts
const fbmInfo = FastNoise.create("FractalFBm").getMembers();
// {
//   type: "FractalFBm",
//   variables: [
//     { name: "Octaves",    type: "int" },
//     { name: "Lacunarity", type: "float" }
//   ],
//   nodes: [
//     { name: "Source" }
//   ],
//   hybrids: [
//     { name: "Gain",              default: 0.5 },
//     { name: "Weighted Strength", default: 0.0 }
//   ]
// }
