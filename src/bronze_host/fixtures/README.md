# bronze_host Integration Test Fixtures

> [!WARNING]
> **DO NOT PUT APPLICATIONS IN THIS DIRECTORY.**
> This folder contains **internal integration test fixtures** used strictly by CTest and regression suites (`tests/bronze_host/`) to verify that the `bronze_host` C++ WebGL2 bindings, timers, frame seams, and microtask checkpoints work correctly.
>
> **Where do real applications go?**
> * Workshop tools and apps belong in `broworkshop/tools/<app-name>/` or `broworkshop/demos/<app-name>/`.
> * Standalone applications belong in their own project repositories with their own `bro.json` and `index.html`.

## Purpose of these fixtures

| Fixture | What it tests |
|---|---|
| `main_scenegraph.js` | Scene graph, matrix math, camera, Object3D hierarchy, timers, rAF, and microtask checkpoint |
| `main.js` | `WebGLRenderer` + `MeshBasicMaterial` cube rendering with framebuffer readback (`gl.readPixels`) |
| `main_lit.js` | `MeshStandardMaterial` + `DirectionalLight` + `AmbientLight` with BRDF shader compiling and uniform bindings |
| `main_textured.js` | `DataTexture` procedural checkerboard generation and WebGL2 texture upload verification |
| `appdir/` | Minimal empty document host harness (`bro.json` + `index.html`) used to boot headless virtual frames |
