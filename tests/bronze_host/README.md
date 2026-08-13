# bronze_host integration check

One check, run by hand: boot `bro-bronze-host` headless, advance a fixed number
of frames, and compare the compiled app's output against
`expected/main_scenegraph.expected` line for line.

```bash
tests/bronze_host/run_bronze_host_test.sh
```

Exit codes are `0` pass, `1` fail, `77` skip (the binary is not built).

## What it actually proves

The subject is `src/bronze_host/app/main_scenegraph.js` compiled to machine code
by bronze and linked into `bro-bronze-host`. Getting one matching line out of it
requires every seam in the layer to be right at once:

- the compiled program's reads of `document`, `window`, `requestAnimationFrame`,
  `performance`, `setTimeout` and `WebGL2RenderingContext` reach the host
  registry — which they only do if the app was compiled against
  `threejs_host.globals` and that file still matches what
  `installThreejsHostGlobals` registers;
- `document.createElement('canvas')` builds a real `dom::Element`,
  `body.appendChild` puts it in the engine's document, and
  `canvas.getContext('webgl2')` reaches `Engine::createWebGL2Context`;
- the width/height setters resize the drawing buffer, and the engine's
  per-frame `syncWebGLCanvasSizes` then leaves it alone;
- `Engine::onFrame` fires the host seam once per `advanceTime` step, and the
  seam runs timers before rAF and the microtask checkpoint after it — the
  `timeout` line lands before frame 0 and each `microtask=N` line lands between
  frame N and frame N+1 for exactly that reason;
- three.js's own arithmetic survives compilation: the scene graph builds and
  `updateMatrixWorld` produces a matrix that is still orthonormal with
  determinant 1 after five frames of accumulated rotation.

A wrong answer on any of them moves or removes a line.

## Why the expectation is only booleans and integers

Same rule as `bronze/tests/oracle/threejs/README.md`. Nothing accumulated is
pinned: the rotation loop accumulates rounding in every matrix element, so what
is checked for it is what stays true of any rotation matrix, inside a tolerance
(1e-12) far wider than double rounding over a few dozen flops and far tighter
than any miscompilation or wrong host value. A pinned float that came out of an
accumulation would be a record of what the build printed, not of what is true.

The GL query in the app prints a boolean for the same reason from the other
direction: `MAX_TEXTURE_SIZE` is the driver's number, so what is pinned is that
the context answered at all.

## Why it is not in `tests/run_tests.sh`

Two reasons, and either alone would be enough:

1. The suite discovers `test_*.js` and evaluates each through `bro-headless`.
   There is no JS realm here to evaluate anything in — the app is a linked
   object file with no scripting surface.
2. `bro-bronze-host` does not exist unless somebody configured
   `-DBRO_WITH_BRONZE=ON` and supplied an app object. Auto-discovering a check
   whose binary is absent from every default build makes "missing" and "broken"
   the same result.

## Getting the binary

`src/bronze_host/README.md` has the sequence: compile the app with bronze's
`--emit-obj --host-globals`, configure bro with `-DBRO_WITH_BRONZE=ON
-DBRO_BRONZE_APP_OBJ=<obj>`, build `bro-bronze-host`.
