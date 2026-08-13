# What `WebGLRenderer` needs that is not vendored

> **Decided: option 2.** `bronze/tests/oracle/threejs/three.module.js` is r160's
> published single-file ESM bundle, vendored byte-for-byte
> (`bronze/tests/oracle/threejs/README.md` has the URL and the sha256), and
> `main.js`, `main_lit.js` and `main_textured.js` beside this file import the
> renderer from it. Nothing about the 28-file tree changed: it is still what
> `oracle-threejs` compiles, still what `main_scenegraph.js` imports, and still
> the only thing that proves module-graph resolution across relative
> specifiers.
>
> The rest of this file is the survey the decision was made from, kept because
> the closure it counts is what makes the answer obvious rather than arbitrary.

**`WebGLRenderer` is not in the tree.** `bronze/tests/oracle/threejs/three/` holds
28 files: the transitive import closure of `Scene`, `PerspectiveCamera`, `Mesh`,
`BoxGeometry` and `MeshBasicMaterial`, and nothing else. There is no
`renderers/` directory in it at all — not `WebGLRenderer.js`, not
`renderers/webgl/`, not `renderers/shaders/`, not `WebGLRenderTarget.js`.

That is why `main.js` beside this file cannot be compiled today and
`main_scenegraph.js` exists.

## What IS vendored

```
constants.js  utils.js
cameras/      Camera.js  PerspectiveCamera.js
core/         BufferAttribute.js  BufferGeometry.js  EventDispatcher.js
              Layers.js  Object3D.js
extras/       DataUtils.js
geometries/   BoxGeometry.js
materials/    Material.js  MeshBasicMaterial.js
math/         Box3.js  Color.js  ColorManagement.js  Euler.js  MathUtils.js
              Matrix3.js  Matrix4.js  Quaternion.js  Ray.js  Sphere.js
              Triangle.js  Vector2.js  Vector3.js
objects/      Mesh.js
scenes/       Scene.js
```

## `WebGLRenderer.js`'s own import list

This is r160's direct import list at the top of `src/renderers/WebGLRenderer.js`.
**It was written from knowledge of the release, not read out of this tree — the
tree does not contain the file.** Check it against a pristine r160 tarball before
acting on it; the shape of the answer (a handful of math modules, then thirty
`renderers/webgl/` helpers) is what matters and is not in doubt.

Already present:

```
../constants.js
../math/Color.js  ../math/Matrix4.js  ../math/Vector2.js  ../math/Vector3.js
../math/ColorManagement.js
../utils.js
```

Absent, directly imported:

```
src/math/Frustum.js
src/math/Vector4.js
src/renderers/WebGLRenderTarget.js
src/renderers/webgl/WebGLAnimation.js
src/renderers/webgl/WebGLAttributes.js
src/renderers/webgl/WebGLBackground.js
src/renderers/webgl/WebGLBindingStates.js
src/renderers/webgl/WebGLBufferRenderer.js
src/renderers/webgl/WebGLCapabilities.js
src/renderers/webgl/WebGLClipping.js
src/renderers/webgl/WebGLCubeMaps.js
src/renderers/webgl/WebGLCubeUVMaps.js
src/renderers/webgl/WebGLExtensions.js
src/renderers/webgl/WebGLGeometries.js
src/renderers/webgl/WebGLIndexedBufferRenderer.js
src/renderers/webgl/WebGLInfo.js
src/renderers/webgl/WebGLMaterials.js
src/renderers/webgl/WebGLMorphtargets.js
src/renderers/webgl/WebGLObjects.js
src/renderers/webgl/WebGLPrograms.js
src/renderers/webgl/WebGLProperties.js
src/renderers/webgl/WebGLRenderLists.js
src/renderers/webgl/WebGLRenderStates.js
src/renderers/webgl/WebGLShadowMap.js
src/renderers/webgl/WebGLState.js
src/renderers/webgl/WebGLTextures.js
src/renderers/webgl/WebGLUniforms.js
src/renderers/webgl/WebGLUniformsGroups.js
src/renderers/webgl/WebGLUtils.js
src/renderers/webxr/WebXRManager.js
```

## The transitive closure, which is the real number

Those thirty pull in most of the library. By directory:

| Group | Roughly | Why it is reached |
|---|---|---|
| `renderers/shaders/ShaderChunk/*.glsl.js` | ~90 | `ShaderChunk` is an index of every GLSL include; `WebGLProgram` resolves `#include <name>` against it |
| `renderers/shaders/ShaderLib/*.glsl.js` | ~30 | one vertex+fragment pair per built-in material |
| `renderers/shaders/{ShaderChunk,ShaderLib,UniformsLib,UniformsUtils}.js` | 4 | the indices themselves |
| `renderers/webgl/*` | 31 | the list above |
| `renderers/webxr/*` | 2–3 | `WebXRManager` and its controller |
| `renderers/WebGL*RenderTarget.js` | 4 | 2D, cube, array, 3D |
| `textures/*` | ~11 | `WebGLTextures` and `WebGLCubeUVMaps` switch on every texture class |
| `materials/*` | ~20 | `WebGLPrograms.getParameters` and `WebGLMaterials` branch on each |
| `lights/*` | ~12 | `WebGLRenderStates` / `WebGLShadowMap` |
| `objects/*` | ~10 | the render list branches on `isPoints`, `isLine`, `isSprite`, `isSkinnedMesh`, `isInstancedMesh`, `isBatchedMesh` |
| `core/*` | ~9 | `RenderTarget`, `Uniform`, `UniformsGroup`, the instanced/interleaved attribute family, `GLBufferAttribute`, `Clock` |
| `cameras/*` | 3 | `ArrayCamera`, `CubeCamera`, `OrthographicCamera` |
| `math/*` | ~5 | `Frustum`, `Plane`, `Vector4`, and what they pull |
| `extras/*` | 2 | `PMREMGenerator`, `ImageUtils` |
| `geometries/PlaneGeometry.js` | 1 | `WebGLBackground`'s fullscreen quad |

**≈200 files, of which ~120 are GLSL string modules.**

## The decision this is here to inform

Three options, and the middle one is almost certainly right:

1. **Vendor the ~200 files.** Preserves the current arrangement (the library at
   the paths its own relative specifiers name, nothing patched) and the current
   argument for it. But it multiplies the milestone's ~70 s compile by the graph
   size, and the compile cost is already why the three.js case lives outside
   `cases/`.

2. **Vendor `build/three.module.js`** — the published single-file ESM bundle,
   ~1.2 MB, byte-for-byte as released. One file, no import graph, still nobody
   on this project's source. It changes what the milestone proves in one way
   worth stating: the module-graph resolution across 28 files stops being
   exercised by the renderer case. The existing 28-file case already proves
   that and should stay exactly as it is, so nothing is lost.

3. **Do not vendor the renderer at all**, and keep `main_scenegraph.js` as the
   host integration proof. The WebGL2 binding surface (`gl_*.cpp`) is then only
   exercised by whatever calls it directly, which is a real gap — the binding
   was written against three.js's usage and has never been driven by it.

Whichever is chosen, `main.js` beside this file is written against option 1 or 2
and needs only its import specifiers repointed.

## The one thing the host layer still owes the renderer

Nothing known. The WebGL2 surface `gl_*.cpp` covers was written from
`src/js/webgl2_bindings*` against three.js's usage, `Image` and the DOM-source
`texImage2D`/`texSubImage2D` overloads landed with this chunk, and the frame
seam performs the microtask checkpoint `setAnimationLoop` needs. The gaps
`../README.md` lists under "Deliberately not covered" are the known ones —
samplers, sync objects, transform feedback, `getContext('2d')` — and none of
them is on the path a `MeshBasicMaterial` cube takes.
