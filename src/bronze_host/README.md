# bronze_host — running bronze-compiled JS apps in bro

The host layer for [bronze](../../../bronze)-compiled (AOT) JavaScript:
registers a browser-shaped set of host globals (`document`, `window`,
`requestAnimationFrame`, …) backed by the engine, and wraps
`webgl::WebGL2RenderingContext` as a bronze object covering the WebGL2
surface three.js r160's renderer drives. `src/js/webgl2_bindings*` is the
reference for that surface and for every GL constant value; this layer
mirrors it, QuickJS values swapped for bronze embed Values.

Off by default; nothing here is in the default build.

## Configure

```bash
cmake -B build -DBRO_WITH_BRONZE=ON            # needs ../bronze sibling checkout
```

Resolves bronze at `../bronze` (`-DBRONZE_DIR=<path>` overrides; a missing
checkout is a configure error — no submodule fallback). bronze's own
configure requires doctest, so the toolchain must provide it (bronze
auto-detects a vcpkg root when bro's configure didn't set one).

## Compile and run an app

```bash
# 1. compile the app against the manifest (bronze repo)
bronze app.js --host-globals src/bronze_host/threejs_host.globals -o app.obj

# 2. link it into the host executable
cmake -B build -DBRO_WITH_BRONZE=ON -DBRO_BRONZE_APP_OBJ=/path/to/app.obj
cmake --build build --config Release --target bro-bronze-host

# 3. run, pointing at a minimal bro app dir (manifest + empty ui page)
./build/Release/bro-bronze-host path/to/appdir
```

The app object must export `bronze_main` (bronze's entry convention);
`installThreejsHostGlobals` runs before `bronze::embed::runMain()`, and
`engine.run()` then drives the app's `requestAnimationFrame` queue.

## Deliberately not covered (yet)

Samplers, sync, occlusion queries, transform feedback, PBO paths,
mapBufferRange, getIndexedParameter, 3D/array textures, non-square matrix
uniforms, `vertexAttrib*` default-value setters, DOM-image texture uploads
(typed arrays only), `getContext('2d')`, and event *dispatch* to
canvas/document listeners (they are stored, never fired). getParameter's
array-shaped answers are pseudo-arrays (indexable, `length`, no
`Array.prototype`).
