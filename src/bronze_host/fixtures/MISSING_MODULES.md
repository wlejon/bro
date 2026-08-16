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
