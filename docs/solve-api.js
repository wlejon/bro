/**
 * =============================================================================
 * bro.solve / solveAll / solveWithCert / prove / fuzz / optimize /
 * superoptimize / rewriteSource / bro.layout.solve — symbolic solving and
 * verification of small JS functions
 * =============================================================================
 *
 * These globals hand a JavaScript function to bronze's checkers
 * (bronze/src/check) and return what they find: argument values that make it
 * return something (solve), a proof report (prove), path-covering test inputs
 * and hazards (fuzz), a cheaper function proven equal to it (optimize,
 * superoptimize, rewriteSource), or a box layout satisfying constraints
 * (bro.layout.solve). They run synchronously on the calling thread. The
 * bindings are src/bronze_host/host_solve.cpp, host_solve_layout.cpp and
 * host_solve_rewrite.cpp; they are installed on the page's `bro` and on a
 * Worker's `bro`.
 *
 * THE FUNCTION ARGUMENT
 *   Every entry point takes a function or a source string. A function is
 *   turned into source with its own toString() and compiled again from that
 *   text, so what is analysed is the text, not the closure:
 *     - variables the function captures are not seen. A free name (one that
 *       is neither a parameter, a binding inside the function nor a known
 *       global such as Math, Number or String) is refused with "free variable
 *       <name>: the function's source is analysed, not its closure; pass the
 *       value as a parameter or inline it" (see ERRORS for how each entry
 *       point reports it). `typeof k` of a free k is refused the same way.
 *     - native functions have no source (`Math.max`): superoptimize and
 *       rewriteSource throw "cannot rewrite native C++ function without JS
 *       source", the others fail to parse the "[native code]" text.
 *   Accepted shapes: arrow functions (`x => e`, `(a, b) => e`,
 *   `(a, b) => { ... }`), `function (a) { ... }`, `function name(a) { ... }`,
 *   with or without `export`, optionally wrapped in parentheses. A source
 *   string may hold several functions; bro.solve picks one by `fnName`,
 *   otherwise the first one is used.
 *
 * THE DOMAIN: 32-BIT SIGNED INTEGERS
 *   Each parameter is a symbolic 32-bit signed integer (int32). Results are
 *   compared as 64-bit integers, so `x >>> 0` (up to 2^32 - 1) and `x * y`
 *   keep their full value; for solve a boolean return is 1 or 0. prove and
 *   optimize also compare the result's kind: a boolean, a number and
 *   undefined (or null) are different results even where their bits agree
 *   (`x > 3` is not `x > 3 ? 1 : 0`, a missing return is not 0). JS semantics are
 *   kept where they differ from machine integers: `x + 1 < x` has no int32
 *   solution (numbers do not wrap), `((x + 1) | 0) < x` does (x = 2147483647).
 *   prove and optimize also accept array and object parameters whose elements
 *   or fields are used as numbers, and prove handles float arithmetic on
 *   integer inputs (`x * 0.5` against `x / 2`).
 *
 *   In scope: + - * / % on integers, `| 0` truncation, bitwise and shift
 *   operators, comparisons, && || !, ?: and if/else, early returns,
 *   Math.abs / Math.imul / Math.trunc / Math.pow with a constant exponent,
 *   indexing a literal array, and loops whose trip count is bounded by the
 *   domain (`for (i = 0; i < n; i++)` with n in [0, 20]).
 *
 *   Rejected (solve, solveWithCert, layout.solve throw
 *   "function not symbolically decidable: <reason>"):
 *     fractional constants ("fractional floating-point constant", including
 *     Math.PI), non-integer division ("non-integer floating-point division"),
 *     BigInt and dynamic calls ("unsupported IL operation: call.dynamic"),
 *     free variables ("free variable k: ..."), and string operations on
 *     the int32 parameters: String(x) and template literals ("string
 *     conversion of a number (String(x), a template literal) is not
 *     modelled"), `'a' + x` ("string concatenation with a number is not
 *     modelled"), and reading a parameter as an object (`x.length`, `x[0]`:
 *     "parameter x is read as a string, array or object (x.length); every
 *     parameter is a 32-bit integer here"). Strings built from literals only
 *     are modelled: `('ab' + 'c').length === x` solves to 3.
 *
 *   Not modelled for prove and optimize, which accept string, array and
 *   object parameters: a parameter's kind is decided by how it is used, so
 *   `String(s).length` is proved as if s were a string. A value stored in an
 *   object or a closure slot and read back loses its boolean kind, and null
 *   is not told from undefined.
 *
 *   The solver can give up: a hard query (`x * x < 0` over all of int32)
 *   ran 640 ms and came back status 'unknown'. Narrowing the domain with
 *   min/max or bounds makes queries smaller and faster.
 *
 * DOMAIN OPTIONS
 *   Every entry point that takes a domain reads `domainMin` / `domainMax`.
 *   solve, solveWithCert, solveAll, prove, optimize and layout.solve also take
 *   the short names `min` / `max` (solve-family reads `min` first, prove and
 *   optimize read `domainMin` first). superoptimize and rewriteSource read
 *   only `domainMin` / `domainMax`. Defaults:
 *     solve, solveWithCert, solveAll, prove, layout.solve  full int32
 *     optimize, superoptimize, rewriteSource               [-10000, 10000]
 *   A proof or rewrite is only claimed over its domain. The function optimize
 *   returns runs the original fn outside it. The source superoptimize and
 *   rewriteSource return is not restricted to it: `x < 0 ? -x : x`
 *   superoptimizes to a branchless abs that is equal on [-10000, 10000] and
 *   differs at -2147483648.
 *
 * ERRORS
 *   Missing first argument, or one that is not a function, object or string:
 *   every entry point throws ("bro.solve: expected a JavaScript function or
 *   source string"). A target that is not a boolean or an integer of
 *   magnitude at most 2^53: solve, solveWithCert and solveAll throw ("target
 *   must be a boolean or an integer of magnitude at most 2^53"). Source that
 *   does not parse or cannot be modelled (a free variable included):
 *     - solve, solveWithCert, prove, fuzz and layout.solve throw with the
 *       compiler's message (prove: "bro.prove: free variable k: ..." for fn,
 *       "bro.prove: equivTo function: free variable k: ..." /
 *       "roundTripWith function: ..." for the others). Positions are in the wrapped source
 *       (`export function __solve_target(x) { return (...); }`), not in the
 *       text you passed: '(x) => x +' reports column 47.
 *     - solveAll returns [].
 *     - optimize and superoptimize return verified: false with the reason
 *       in `summary`. A candidate with a free variable is never proven.
 *     - rewriteSource returns the source unchanged with summary
 *       "Source does not parse; no rewrites applied".
 */

// ── Solving ──────────────────────────────────────────────────────────────────

/**
 * @typedef {Object} SolveOptions
 * @property {number|boolean} [target]  Find arguments where the return value
 *   equals this (true = 1, false = 0). Without it the goal is a nonzero
 *   (truthy) return. Compared as a 64-bit integer, so 4294967295 matches
 *   `x >>> 0` at x = -1 and 3000000000 matches no int32 `x`. A fractional,
 *   non-finite or non-number target, or one beyond 2^53, throws.
 * @property {number} [min]  Lower bound for every parameter (alias domainMin).
 * @property {number} [max]  Upper bound for every parameter (alias domainMax).
 * @property {number} [bitWidth]  1..31: sets min/max to the signed range of
 *   that width ([-128, 127] for 8), or [0, 2^bitWidth - 1] with
 *   `unsigned: true`. An explicit min or max wins over it.
 * @property {boolean} [unsigned]  With bitWidth, use the unsigned range.
 * @property {Object<string, [number, number]|{min?: number, max?: number}>} [bounds]
 *   Per-parameter bounds by parameter name, intersected with min/max.
 * @property {boolean} [minimize]  After a solution is found, try to replace
 *   each argument, in order, with a value of smaller magnitude from the fixed
 *   list 0, ±1..±4, 5..10, 12, 16, 20, 24, 32, 42, 50, 64, 100, keeping the
 *   first that still satisfies the goal. It is not a search for the minimum:
 *   for `x > 10 && x % 7 === 3` it returns 24, not 17.
 * @property {string} [fnName]  Which function of a multi-function source
 *   string to solve (bro.solve, solveWithCert, solveAll only).
 */

/**
 * Find one argument tuple for which `fn` returns nonzero (or `opts.target`).
 *
 * Returns null when there is none in the domain, and also when the solver
 * gives up (use solveWithCert to tell those apart). Otherwise an Array of the
 * arguments in parameter order, with extra properties:
 *   - one property per parameter name (`r.x`); a parameter named `length`
 *     is left out (it would clobber the array's length), see `model`
 *   - `model`        {name: value} for every parameter
 *   - `returnValue`  what fn returns for these arguments (number; 1/0 for
 *                    a boolean)
 *   - `solveTimeMs`  wall time of the query
 *   - `status`       'sat'
 * Throws when fn cannot be modelled (see header).
 *
 * @param {Function|string} fn
 * @param {SolveOptions} [opts]
 * @returns {(number[] & {model: Object<string, number>, returnValue: number,
 *            solveTimeMs: number, status: 'sat'})|null}
 */
bro.solve = function (fn, opts) {};

/**
 * Same query as bro.solve, always returning an object, so an empty domain
 * and a solver that gave up are distinguishable.
 *   status       'sat' | 'unsat' | 'unknown'
 *   sat, unsat   booleans for the first two
 *   solveTimeMs  wall time
 *   args         argument array, or null unless sat
 *   model        {name: value}, or null unless sat
 *   returnValue  number, or undefined unless sat
 * Throws when fn cannot be modelled.
 *
 * @param {Function|string} fn
 * @param {SolveOptions} [opts]
 * @returns {{status: 'sat'|'unsat'|'unknown', sat: boolean, unsat: boolean,
 *            solveTimeMs: number, args: number[]|null,
 *            model: Object<string, number>|null, returnValue: number|undefined}}
 */
bro.solveWithCert = function (fn, opts) {};

/**
 * Up to `maxSolutions` distinct argument tuples (default 10), each found by
 * re-solving with the previous tuples excluded. Each element is an Array of
 * arguments with one property per parameter name (no model / returnValue).
 * A zero-parameter function yields at most one (empty) tuple.
 *
 * Two call forms: (fn, maxSolutions, opts) or (fn, opts) with
 * `opts.maxSolutions`. A count of 0 or less returns [].
 *
 * Never throws for an unmodellable function: it returns []. It also stops
 * without saying so when one query comes back 'unknown', so fewer results
 * than asked does not prove there are no more: `x * x < 10` over full int32
 * returned 6 of its 7 solutions, and all 7 with {min: -100, max: 100}.
 *
 * @param {Function|string} fn
 * @param {number|(SolveOptions & {maxSolutions?: number})} [maxSolutions=10]
 * @param {SolveOptions} [opts]
 * @returns {Array<number[] & Object<string, number>>}
 */
bro.solveAll = function (fn, maxSolutions, opts) {};

// ── Verification ─────────────────────────────────────────────────────────────

/**
 * @typedef {Object} ProveOptions
 * @property {number} [domainMin]  Lower bound of every parameter for the
 *   equivalence and round-trip proofs (alias min). Full int32 by default.
 * @property {number} [domainMax]  Upper bound (alias max).
 * @property {Function|string} [equivTo]  Prove fn returns the same value as
 *   this function for every input in the domain (alias equivalentTo). Its
 *   name does not matter.
 * @property {Function|string} [roundTripWith]  Prove inverse(fn(x)) === x for
 *   every x in the domain (alias inverseFn).
 * @property {boolean} [synthesizeTests=true]  Also generate path-covering test
 *   inputs (`synthesizedTests`); a hazard they reproduce is added to `hazards`.
 */

/**
 * Run bronze's checks on fn and report:
 *   1. static hazards (division by a value that can be zero, out-of-bounds
 *      index, null dereference, NaN sources) from the abstract interpreter
 *   2. replay hazards: wall-clock time, unseeded Math.random, iteration order
 *      and outer state (the lockstep audit used for deterministic netcode)
 *   3. whether the compiled IL and its guard-elimination proofs verify
 *   4. with equivTo: equivalence over the int32 domain
 *   5. with roundTripWith: decode(encode(x)) === x over the domain
 *   6. with synthesizeTests: SMT-generated inputs covering each path
 *
 * Result:
 *   status            'VERIFIED' when every check passed, else 'UNVERIFIED'
 *   verified          the same as a boolean
 *   safe, hazardFree  no static or reproduced hazard
 *   deterministic     no lockstep hazard
 *   proofsValid       IL and proof verification passed
 *   provenGuards, totalGuards  type/bounds guards the compiler proved away,
 *                     of those it counted; unboxedF64Ops: unboxed numeric
 *                     instructions. Compiler statistics, not proof results.
 *   equivalent        true when equivTo was not given; false both for a
 *                     counterexample and for a proof that could not be
 *                     decided (then counterexample is null; seen for `x / 3`
 *                     against `x * (1/3)` and for string parameters)
 *   equivalenceDetail the prover's sentence on the equivalence ('' without
 *                     equivTo), e.g. why it could not decide, or "counterexample
 *                     found where the old function returns a boolean and the
 *                     new one a number"
 *   roundTripProven   true when roundTripWith was not given
 *   roundTripDetail   the same for the round trip
 *   counterexample    {args, witnessArgs, expectedOutput, actualOutput} or
 *                     null. expectedOutput is fn's result, actualOutput the
 *                     other function's (round trip: the input and what came
 *                     back). For a float-valued function the args and
 *                     outputs are the raw IEEE-754 bit patterns as numbers
 *                     (1.0 shows as 4607182418800017400), rounded to a double.
 *   hazards           strings 'kind: message'
 *   lockstepHazards   strings 'kind: message'
 *   synthesizedTests  [{args, expectedReturn, kind: 'branch-coverage' |
 *                     'hazard-reproducer', pathDescription}]
 *   sha256, proofHash the SHA-256 (hex) of the normalized source, the text in
 *                     `export function <name>(params) { ... }` form that every
 *                     check ran on (for `(x) => x`:
 *                     'export function __prove_target(x) { return (x); }')
 *   certificateJson   the booleans and counts above as JSON, schema
 *                     'bronze.proof.cert.v1'
 *
 * The equivalence compares the value and its kind (boolean, number,
 * undefined): `x => x > 3` is not equivalent to `x => x > 3 ? 1 : 0`, and
 * the counterexample is any input. Throws on source that does not parse or
 * compile, and on a free variable in fn, equivTo or roundTripWith.
 *
 * @param {Function|string} fn
 * @param {ProveOptions} [opts]
 * @returns {Object}
 */
bro.prove = function (fn, opts) {};

/**
 * Concolic path exploration: SMT-solved inputs that reach each branch, the
 * value fn returns for each, and hazards with inputs that trigger them.
 *
 * Options: `maxPaths` (default 64) caps the number of testVectors returned.
 *
 * Result:
 *   functionName     the function's name ('__fuzz_target' for an anonymous
 *                    or arrow function)
 *   branchCoverage   coveredBranches / totalBranches (basic blocks)
 *   totalBranches, coveredBranches
 *   feasiblePaths    testVectors.length (after the maxPaths cap)
 *   safe             hazards.length === 0
 *   testVectors      [{args, output, targetBlockId, branchPath}] where
 *                    branchPath reads like 'b0->T(b1)->F(b4)' (T/F: the
 *                    branch taken). Each entry also carries input (= args),
 *                    returnValue (= output) and path (= branchPath).
 *   tests            the same array as testVectors
 *   hazards          [{kind, message, line, col, witnessArgs}]; kind is
 *                    'div-by-zero' | 'out-of-bounds' | 'null-deref' |
 *                    'nan-poison' | 'exception' | 'hazard' or a static
 *                    checker tag. Also description (= message), witness
 *                    (= witnessArgs). A hazard found only by the static
 *                    checker has no input of its own: its witnessArgs are
 *                    copied from the first test vector or are empty, and its
 *                    line/col count in the wrapped source (0 for SMT ones).
 *   bugs             the same array as hazards
 *   json             the report as a JSON string
 *
 * Coverage below 1 means some blocks were not reached with int32 inputs
 * (a float comparison or a string parameter, for example).
 * Throws on source that does not parse, and on a free variable.
 *
 * @param {Function|string} fn
 * @param {{maxPaths?: number}} [opts]
 * @returns {Object}
 */
bro.fuzz = function (fn, opts) {};

// ── Optimization ─────────────────────────────────────────────────────────────

/**
 * Counterexample-guided search for a cheaper function equal to fn on
 * [domainMin, domainMax] (default [-10000, 10000]). Candidates are tried in
 * order, each one checked for equivalence by the SMT solver:
 *   1. opts.candidates (functions or strings; a named one is renamed to fn's
 *      name in optimizedSource)
 *   2. built-in rewrites of fn's text: ` * 2^k` to ` << k`, ` % 2^k` to
 *      ` & (2^k-1)`, `(x / 2^k) | 0` to `>> k`, Math.pow(x, 2|3) to products,
 *      Math.abs(x) to a branchless form, and for a loop the closed forms of
 *      0+1+...+(n-1) and 0+1+...+n
 *   3. after a counterexample: opts.proposeCandidate(failedSource, cex) if
 *      given (its return, a function or string, is tried next), else with
 *      autoPatchCounterexamples (default true) the same candidate with
 *      `if (x === <cex>) return <expected>;` prepended, for at most 4
 *      counterexamples
 *   4. when neither candidates nor proposeCandidate were given and nothing
 *      matched: the enumerative superoptimizer (bro.superoptimize)
 * The search stops at the first proven candidate or after maxRounds
 * (default 16) checks. proposeCandidate is also called once up front, with
 * an empty cex, when there are no candidates at all. Identical candidates are
 * not merged: a rule that produces the same text as a user candidate is
 * checked again.
 *
 * Options: domainMin/min, domainMax/max, maxRounds, autoPatchCounterexamples,
 * candidates (up to 64), proposeCandidate(failedSource, cex) where cex is
 * {round, args, witnessArgs, expectedOutput, actualOutput, candidateOutput}.
 *
 * Returns a callable function. When a different source was proven
 * (verified and optimized) and every argument is an integer number in
 * [domainMin, domainMax] (not -0), one per declared parameter, it evaluates
 * optimizedSource with the solver's concrete evaluator (compiling the source
 * each call) and returns its result as the kind it is: a boolean for a
 * boolean, a number for a number. Every other call (no proven rewrite, an
 * argument outside the domain, a non-integer or non-number, too few
 * arguments, an undefined or null result, a source the evaluator cannot
 * run) calls the original fn with the arguments as given, so the callable
 * returns what fn returns. Its properties:
 *   verified, provenEquivalent  the returned source is proven equal to fn on
 *                     the domain. With no candidate found, this says whether
 *                     fn itself could be modelled (it is checked against
 *                     itself)
 *   optimized         a different source was proven equal
 *   optimizedSource   normalized source (fn's own when not optimized)
 *   referenceSource   fn's normalized source
 *   summary           one line; on failure the reason
 *   roundsUsed, iterations  candidates checked
 *   counterexamplesFixed    counterexamples.length
 *   counterexamples   [{round, args, witnessArgs, expectedOutput,
 *                     actualOutput, candidateOutput}]
 *   certificate       {verified, provenEquivalent, optimized, roundsUsed,
 *                     optimizedSource, summary}
 *   fn, evalNative    the callable itself
 * Never throws for an unmodellable fn; it returns verified: false.
 *
 * @param {Function|string} fn
 * @param {Object} [opts]
 * @returns {Function & Object}
 */
bro.optimize = function (fn, opts) {};

/**
 * Enumerative superoptimizer: looks for the lowest-cost single expression
 * (templates such as branchless abs/min/max/sign and masks, then bottom-up
 * enumeration of small expression DAGs) that agrees with fn on a growing
 * bank of test inputs, and proves the survivor equal on
 * [domainMin, domainMax] (default [-10000, 10000]).
 *
 * Options: domainMin, domainMax (no min/max aliases), maxCandidates (default
 * 2048; caps the template list only, the bottom-up enumeration that follows
 * is not capped by it).
 *
 * Result:
 *   verified, optimized       proven equal / lower cost than fn
 *   functionName              '__superopt_fn' for an anonymous function
 *   originalSource            the text passed in
 *   synthesizedExpression     the expression found, '' if none
 *   optimizedSource           `function <name>(params) { return <expr>; }`
 *   ruleDescription           the template's name, e.g. "max(x, y)"
 *   certificateHash           '0x' + 16 hex digits, '' if none
 *   summary                   one line; on failure the reason
 *   stats  {candidatesGenerated, candidatesTested, concretePruned,
 *           smtQueries, counterexamplesFound, seedVectorCount,
 *           finalVectorCount, costBefore, costAfter, elapsedMs}
 * An expression of equal cost is returned with verified: true and
 * optimized: false. Throws only for a missing argument or a native function;
 * a parse or modelling failure comes back as verified: false.
 *
 * @param {Function|string} fn
 * @param {{domainMin?: number, domainMax?: number, maxCandidates?: number}} [opts]
 * @returns {Object}
 */
bro.superoptimize = function (fn, opts) {};

/**
 * Rewrite a whole source text: dead-branch and redundant-guard elimination,
 * strength reduction, Horner factoring of polynomials, fusing
 * `.map().filter().reduce()` over a plain Array into one loop (guarded, so
 * other receivers run the original chain), and superoptimization of
 * numeric functions. Each numeric rewrite must be proven equal on
 * [domainMin, domainMax] (default [-10000, 10000]).
 *
 * Works on function declarations (`function name(...) {...}`); a bare arrow
 * function passed in was returned unchanged.
 *
 * Options: domainMin, domainMax, requireFormalProof (default true).
 *
 * Result:
 *   originalSource, rewrittenSource
 *   unifiedDiff        a diff of the two texts. It pairs lines by position
 *                      after a change, so when a rewrite adds lines, the
 *                      unchanged lines after it show as removed and re-added.
 *   bytesBefore, bytesAfter, estimatedOpsBefore, estimatedOpsAfter
 *   allProvenEquivalent
 *   certificateHash    '0x' + 16 hex digits over the whole result
 *   summary            one line
 *   cegisStats         as superoptimize's stats
 *   rewrites           [{functionName, ruleName ('cegis-superopt',
 *                      'loop-deforestation', ...), description,
 *                      beforeSnippet, afterSnippet, provenEquivalent,
 *                      certificateHash}]
 * Throws only for a missing argument or a native function.
 *
 * @param {Function|string} source
 * @param {{domainMin?: number, domainMax?: number, requireFormalProof?: boolean}} [opts]
 * @returns {Object}
 */
bro.rewriteSource = function (source, opts) {};

// ── Layout ───────────────────────────────────────────────────────────────────

/** Always true. @type {boolean} */
bro.layout.available;

/**
 * Integer layout solving. Three call forms:
 *
 * 1. bro.layout.solve(spec, [predicate])
 *    spec = {
 *      container: {w|width, h|height, d|depth} (default 1920 x 1080 x 1000),
 *                 or false for no container,
 *      nonOverlap: boolean   no two boxes overlap (with `gap` between them),
 *      gap: number,
 *      snapGrid | grid: n    x, y, w, h (and z, d) are multiples of n,
 *      boxes: [{id, ...}] (up to 32; id defaults to 'box_<i>') or {id: {...}},
 *             or the boxes as top-level keys of spec itself,
 *      constraints: [string] (up to 128) or a predicate function,
 *    }
 *    A box: x, y, z, w|width, h|height, d fixes a value; minX/maxX, minY/maxY,
 *    minZ/maxZ, minW/maxW (or minWidth/maxWidth), minH/maxH (or
 *    minHeight/maxHeight), minD/maxD give ranges (default x, y, z in
 *    [0, 4096], sizes in [1, 4096], clipped to the container);
 *    snapGrid | grid per box; aspectRatio: [num, den] or aspectNum/aspectDen
 *    (w * den === h * num). Any z/d/minZ/minD, or is3D: true, makes it 3D.
 *    Constraint strings use `id.x` / `id_x` and x y z w h d, left top
 *    width height depth right bottom centerX centerY, `container.w|h|d`,
 *    `gap`, integer literals, + - * / %, & | ^ << >>, comparisons
 *    (== and === are the same), && || ! and parentheses. A string that does
 *    not parse throws "invalid layout constraint expression: '<text>'".
 *    The predicate (second argument, or a function in `constraints`) receives
 *    the box variables by NAME: a parameter `<id>_<x|y|w|h>` (z, d for a 3D
 *    box) is that box's variable wherever it stands, so `(b_y, a_x) => ...`
 *    gets b.y and a.x. A parameter that names no box variable throws
 *    "bro.layout.solve: predicate parameter 'c_x' names no box variable: name
 *    each parameter <box>_<x|y|w|h> (z, d for a 3D box) after a box of the
 *    spec (a, b)". The predicate is solved
 *    like bro.solve: its parameters are int32, and the same constructs are
 *    refused.
 *
 * 2. bro.layout.solve(varNames, predicate, [opts])
 *    Solves predicate(...vars) like bro.solve, and names the results with
 *    varNames (by position). A name `<id>_<x|y|z|w|h|d>` or `<id>.<prop>` also
 *    fills boxes[id]. opts: min/domainMin, max/domainMax, minimize, bounds
 *    (by parameter name) as in SolveOptions; no target.
 *
 * 3. bro.layout.solve(predicate, [opts])
 *    As 2, named by the predicate's parameter names.
 *
 * Returns null when unsatisfiable or when the solver gave up; otherwise
 *   {status: 'sat', sat: true, solveTimeMs,
 *    vars: {name: value}   form 1 lists each box value as both 'a.x' and 'a_x',
 *    boxes: {id: box},
 *    <id>: box             each box again at the top level,
 *    <name>: value         each var whose name has no '.'}
 *   box = {id, x, y, w, h, width, height, right, bottom} plus z, d, depth
 *   for a 3D box. In forms 2 and 3 a box field no variable named is 0.
 * Throws for a first argument that is not an array, object, function or
 * string, for form 2 without a predicate, and for a predicate that cannot be
 * modelled.
 *
 * @param {Object|string[]|Function|string} specOrVars
 * @param {Function|string|Object} [predicateOrOpts]
 * @param {Object} [opts]
 * @returns {Object|null}
 */
bro.layout.solve = function (specOrVars, predicateOrOpts, opts) {};

// ── Examples ─────────────────────────────────────────────────────────────────
// Each was run through bro-headless; the comment is what it returned (the
// "Refused rather than answered" block: what the soundness test asserts).

/**
 * @example
 *   // One solution, as an array with named fields
 *   const r = bro.solve((x, y) => x + y === 10 && x - y === 4);
 *   // [...r] → [7, 3]; r.x → 7; r.y → 3; r.model → {x: 7, y: 3};
 *   // r.returnValue → 1; r.status → 'sat'
 *
 *   bro.solve((x) => x * 0 === 1);                        // → null
 *   bro.solve((x) => x * 3, { target: 21 }).x;            // → 7
 *   [...bro.solve((x, y) => x * y === 12,
 *       { bounds: { x: [2, 3], y: { min: 0, max: 10 } } })];   // → [3, 4]
 *   bro.solve((x) => x > 200, { bitWidth: 8, unsigned: true }).x;   // → 201
 *   bro.solve((x) => x < -100, { bitWidth: 8 }).x;        // → -128
 *   [...bro.solve('function g(a) { return a === 1; } function h(b) { return b === 2; }',
 *       { fnName: 'h' })];                                 // → [2]
 *   bro.solve((x) => x / 2 === 1.5);
 *   // throws "bro.solve: function not symbolically decidable:
 *   //         non-integer floating-point division"
 *
 * @example
 *   // Refused rather than answered, and full-width targets (what
 *   // tests/math/test_solver_soundness.js asserts)
 *   bro.solve((x) => String(x).length === 2);
 *   // throws "bro.solve: function not symbolically decidable: string conversion
 *   //         of a number (String(x), a template literal) is not modelled"
 *   const k = 4;
 *   bro.solveWithCert((x) => x === k);
 *   // throws "bro.solveWithCert: function not symbolically decidable: free
 *   //         variable k: the function's source is analysed, not its closure;
 *   //         pass the value as a parameter or inline it"
 *   [...bro.solve((x) => x >>> 0, { target: 4294967295 })];   // → [-1]
 *   bro.solve((x) => x, { target: 3000000000 });               // → null
 *   bro.prove((x) => x > 3, { equivTo: (x) => (x > 3 ? 1 : 0),
 *       synthesizeTests: false }).equivalent;                  // → false
 *   bro.optimize((x) => x > 3)(5);                             // → true
 *   bro.optimize((s) => s.length)('abcd');                     // → 4
 *
 * @example
 *   // Unsat and unknown kept apart
 *   // (solveTimeMs varies from run to run)
 *   bro.solveWithCert((x) => x > 5 && x < 3);
 *   // → {status: 'unsat', sat: false, unsat: true, solveTimeMs: 0.418936,
 *   //    args: null, model: null}
 *   bro.solveWithCert((x, y) => x * y === 6 && x > y && y > 0);
 *   // → {status: 'sat', sat: true, unsat: false, solveTimeMs: 0.315942,
 *   //    args: [6, 1], model: {x: 6, y: 1}, returnValue: 1}
 *
 * @example
 *   // Several solutions
 *   bro.solveAll((x, y) => x + y === 3, 4, { min: 0, max: 3 })
 *     .map(r => [r.x, r.y]);
 *   // → [[3, 0], [0, 3], [2, 1], [1, 2]]
 *   bro.solveAll((x) => x > 0 && x < 100, { maxSolutions: 3, minimize: true })
 *     .map(r => r[0]);                                    // → [1, 2, 3]
 *
 * @example
 *   // Equivalence with a counterexample, then on a narrower domain
 *   const p = bro.prove((x) => (x / 2) | 0,
 *       { equivTo: (x) => x >> 1, synthesizeTests: false });
 *   // p.status → 'UNVERIFIED'; p.equivalent → false;
 *   // p.counterexample → {args: [-1], witnessArgs: [-1],
 *   //                     expectedOutput: 0, actualOutput: -1}
 *   bro.prove((x) => (x / 2) | 0,
 *       { equivTo: (x) => x >> 1, min: 0, max: 1000, synthesizeTests: false }).status;
 *   // → 'VERIFIED'
 *
 *   // A loop against its closed form
 *   bro.prove((n) => { let s = 0; for (let i = 0; i < n; i++) s += i; return s; },
 *       { equivTo: (n) => n <= 0 ? 0 : (n * (n - 1)) / 2, min: 0, max: 1000,
 *         synthesizeTests: false }).equivalent;          // → true
 *
 *   // Round trip
 *   bro.prove((x) => x ^ 0x5a5a, { roundTripWith: (y) => y ^ 0x5a5a,
 *       synthesizeTests: false }).roundTripProven;       // → true
 *   bro.prove((x) => (x * 3) | 0, { inverseFn: (y) => (y / 3) | 0,
 *       synthesizeTests: false }).counterexample;
 *   // → {args: [1073741823], witnessArgs: [1073741823],
 *   //    expectedOutput: 1073741823, actualOutput: -357913942}
 *
 * @example
 *   // Hazards and replay determinism
 *   const h = bro.prove((x) => 10 / x);
 *   // h.safe → false; h.hazards →
 *   //  ["nan-poison: [div-by-zero] unguarded division by potentially zero 'x' produces NaN or Infinity",
 *   //   "smt-hazard: division by zero (divisor == 0 -> Infinity/NaN)",
 *   //   "smt-hazard: [div-by-zero] unguarded division by potentially zero 'x' produces NaN or Infinity"]
 *   // h.synthesizedTests[0] → {args: [0], expectedReturn: 0,
 *   //                          kind: 'hazard-reproducer', pathDescription: 'b0'}
 *   const d = bro.prove(() => Math.random());
 *   // d.deterministic → false; d.verified → false; d.lockstepHazards →
 *   //  ['unseeded-prng: Math.random() is non-deterministic across lockstep peers and replays']
 *
 * @example
 *   // Path-covering inputs
 *   const f = bro.fuzz((x, y) => { if (x > 10) { if (y === x * 2) return 1; return 2; } return 3; });
 *   // f.branchCoverage → 1; f.totalBranches → 5; f.safe → true;
 *   // f.testVectors.map(t => [t.args, t.output, t.branchPath]) →
 *   //  [[[16, 32], 1, 'b0->T(b1)->T(b3)'], [[42, 1], 2, 'b0->T(b1)->F(b4)'],
 *   //   [[1, 1], 3, 'b0->F(b2)']]
 *   bro.fuzz((i) => { const a = [1, 2, 3]; return a[i]; }).hazards[0];
 *   // → {kind: 'out-of-bounds', message: 'out-of-bounds array index (index < 0 or index >= 3)',
 *   //    description: <same>, line: 0, col: 0, witnessArgs: [3], witness: [3]}
 *
 * @example
 *   // optimize: built-in rewrite, user candidate, counterexample patching
 *   const m = bro.optimize((x) => x * 8 | 0);
 *   // m.optimizedSource → 'export function __opt_target(x) { return (x << 3 | 0); }'
 *   // m.summary → 'CEGIS proved candidate equivalent in 1 round(s) across [-10000, 10000]'
 *   // m(5) → 40
 *   bro.optimize((x) => (x % 8 + 8) % 8,
 *       { candidates: [(x) => x & 7], min: -1000, max: 1000 }).optimizedSource;
 *   // → 'export function __opt_target(x) { return (x & 7); }'
 *   bro.optimize((x) => x % 8,
 *       { candidates: [(x) => x & 7], domainMin: -2, domainMax: 100 }).optimizedSource;
 *   // → 'export function __opt_target(x) { if (x === -2) return -2;
 *   //     if (x === -1) return -1; return (x & 7); }'
 *   const s = bro.optimize((n) => { let s = 0; for (let i = 0; i < n; i++) s += i; return s; },
 *       { min: 0, max: 1000 });
 *   // s.optimizedSource → 'export function __opt_target(n) { return n <= 0 ? 0 :
 *   //                      (((n * (n - 1)) / 2) | 0); }'; s(10) → 45
 *
 *   // proposeCandidate gets each counterexample
 *   bro.optimize((x) => x * 3 | 0, { candidates: [(x) => x + x], maxRounds: 4,
 *       proposeCandidate: (prev, cex) => (x) => (x + x + x) | 0 });
 *   // the callback was called once with {round: 1, args: [1], witnessArgs: [1],
 *   //   expectedOutput: 3, actualOutput: 2, candidateOutput: 2};
 *   // optimizedSource → 'export function __opt_target(x) { return ((x + x + x) | 0); }'
 *
 *   bro.optimize((x) => x * 0.5).summary;
 *   // → 'reference function could not be symbolically verified: old function not
 *   //    symbolically decidable: fractional floating-point constant'
 *
 * @example
 *   // superoptimize
 *   const a = bro.superoptimize((x) => x < 0 ? -x : x);
 *   // a.synthesizedExpression → '((x ^ (x >> 31)) - (x >> 31))'
 *   // a.ruleDescription → "Hacker's Delight branchless abs(x)"
 *   // a.stats.costBefore → 8; a.stats.costAfter → 3; a.verified → true
 *   bro.superoptimize((x) => x % 2 === 0 ? 0 : 1,
 *       { domainMin: 0, domainMax: 1000 }).synthesizedExpression;   // → '(x & 1)'
 *
 * @example
 *   // rewriteSource
 *   const w = bro.rewriteSource(function mod8(x) { return x % 8; },
 *       { domainMin: 0, domainMax: 1000 });
 *   // w.rewrittenSource → 'function mod8(x) {\n  return (x & 7);\n}'
 *   // w.rewrites[0].ruleName → 'cegis-superopt'; .provenEquivalent → true
 *   // w.summary → '1 verified rewrite pass(es) applied; bytes 34 -> 38, ops 3 -> 1
 *   //              (cert 0x882dbd3fd2e26f03)'
 *   bro.rewriteSource('function f(x) { return x + ; }').summary;
 *   // → 'Source does not parse; no rewrites applied'
 *
 * @example
 *   // Layout from a spec
 *   const L = bro.layout.solve({
 *     container: { width: 400, height: 300 },
 *     nonOverlap: true, gap: 10, grid: 10,
 *     boxes: [{ id: 'a', w: 200, h: 100 }, { id: 'b', width: 150, minH: 50, maxH: 80 }],
 *     constraints: ['a.x == 0', 'b.y >= a.y'],
 *   });
 *   // L.a → {id: 'a', x: 0, y: 0, w: 200, h: 100, width: 200, height: 100,
 *   //        right: 200, bottom: 100}
 *   // L.b → {id: 'b', x: 0, y: 160, w: 150, h: 70, width: 150, height: 70,
 *   //        right: 150, bottom: 230}
 *   // L.vars['b.y'] → 160; L.b_y → 160
 *   bro.layout.solve({ container: { w: 100, h: 100 }, nonOverlap: true,
 *       boxes: [{ id: 'a', w: 80, h: 80 }, { id: 'b', w: 80, h: 80 }] });   // → null
 *   bro.layout.solve({ container: { w: 500, h: 500 },
 *       boxes: [{ id: 'v', minW: 100, maxW: 300, aspectRatio: [16, 9] }] }).v;
 *   // → {id: 'v', x: 0, y: 0, w: 256, h: 144, ...}
 *
 *   // Layout from variables and a predicate
 *   bro.layout.solve(['a_x', 'a_w'], (a_x, a_w) => a_x + a_w === 100 && a_w === 40,
 *       { min: 0, max: 1000 }).a;
 *   // → {id: 'a', x: 60, y: 0, w: 40, h: 0, width: 40, height: 0, right: 100, bottom: 0}
 *   const v = bro.layout.solve((w, h) => w * 9 === h * 16 && w > 100, { min: 0, max: 2000 });
 *   // v.w → 1024; v.h → 576; v.vars → {h: 576, w: 1024}; v.boxes → {}
 */
