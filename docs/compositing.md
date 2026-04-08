# Compositing Layer System

The engine composites HTML/CSS and Canvas/WebGL content as interleaved layers in DOM order. This ensures correct stacking: HTML elements that come after a canvas in the document paint on top of it, and canvas elements paint on top of preceding HTML.

## Problem

A browser paints elements in document order within their stacking context. A `<canvas>` is just another element — HTML with `position: absolute` later in the DOM naturally renders on top.

Bro renders HTML/CSS to a Skia surface and Canvas 2D to separate Skia surfaces. Without a layer system, these are composited as flat textures in a fixed order — canvas is either always above or always below all HTML. Neither is correct for apps that mix the two (e.g., a game canvas with HTML overlay menus).

## Design

During the draw traversal, each `<canvas>` element triggers a **layer break**. The HTML content before the canvas becomes one layer, the canvas itself is a layer, and the HTML after becomes another layer. The result is an ordered list:

```
[HTML Layer 0] [Canvas Layer 0] [HTML Layer 1] [Canvas Layer 1] [HTML Layer 2]
```

Each HTML layer is a full-viewport GPU-backed Ganesh surface (transparent where nothing is drawn). Each canvas layer references the `CanvasScene`'s own Skia surface at its layout position. During GL compositing, layers are drawn in order as textured quads with alpha blending.

### Example: Tetris

```html
<canvas id="game"></canvas>          <!-- game board -->
<div id="hud">...</div>             <!-- score display (position: absolute) -->
<div id="overlay">...</div>         <!-- pause/menu screen (position: absolute) -->
```

Layers produced:
1. **HTML Layer 0** — empty (nothing before the canvas)
2. **Canvas Layer 0** — the game board
3. **HTML Layer 1** — HUD + overlay menus

The overlay composites on top of the canvas. Correct.

### Example: Multi-Canvas

```html
<h1>Multi Canvas Test</h1>
<div class="canvas-row">
    <div><h2>Canvas A</h2><canvas id="a"></canvas></div>
    <div><h2>Canvas B</h2><canvas id="b"></canvas></div>
</div>
```

Layers produced:
1. **HTML Layer 0** — title + "Canvas A" label
2. **Canvas Layer 0** — canvas A content
3. **HTML Layer 1** — "Canvas B" label
4. **Canvas Layer 1** — canvas B content
5. **HTML Layer 2** — any remaining HTML

Each canvas appears in its correct position within the layout.

## Performance

### GPU-backed HTML layers

HTML layer surfaces are GPU-backed Ganesh surfaces — each pool entry owns an FBO + GL texture. Skia draw calls go directly to GPU via the Ganesh GL backend. There is no CPU→GPU pixel upload.

**Important:** `WrapBackendRenderTarget` surfaces cannot be reused across Ganesh flush boundaries. Each dirty frame calls `rewrapGPUSurface()` to create a fresh SkSurface wrapper around the same FBO/texture. The GL resources (FBO, texture) are pooled and reused; only the cheap Skia wrapper is recreated.

### HTML raster throttle

HTML rasterization is throttled to `kUIFrameIntervalMs` (8ms, ~120fps). Canvas/WebGL layers rasterize every frame. The throttle check:

```cpp
bool uiThrottled = (now - lastUIRenderMs_ < kUIFrameIntervalMs);
if ((uiDirty_ || !hasRenderedOnce_) && !uiThrottled) { ... }
```

### Canvas-only updates skip HTML re-rasterization

When a canvas animates (e.g., waveform at 360fps), the engine detects that only canvas content changed (no DOM mutations). The HTML layer textures from the previous frame are still valid. Only canvas textures are re-rasterized. The compositing pass draws cached HTML textures + fresh canvas textures.

Full HTML re-rasterization only happens when the DOM changes AND 8ms have elapsed since the last raster.

### Per-surface Ganesh flush

Before `endFrame()`, each pool surface's deferred Ganesh ops are explicitly flushed via `grContext_->flush(surface)`. Then `endFrame()` calls `grContext_->flushAndSubmit()` to submit all work to the GPU.

## Implementation

### Key components

| Component | File | Role |
|-----------|------|------|
| `LayerBreakCallback` | `layout/draw_traversal.h` | Callback fired when the traversal encounters a `<canvas>` |
| `SkiaRenderer::switchSurface()` | `render/skia_backend.cpp` | Swaps the active Skia surface mid-frame for layer splitting |
| `SkiaRenderer::GPUSurface` | `render/skia_backend.h` | Struct: `sk_sp<SkSurface>` + `GLuint texture` + `GLuint fbo` |
| `SkiaRenderer::createGPUSurface()` | `render/skia_backend.cpp` | Creates FBO + texture + Ganesh surface |
| `SkiaRenderer::rewrapGPUSurface()` | `render/skia_backend.cpp` | Recreates SkSurface wrapper for existing FBO/texture |
| `SkiaRenderer::destroyGPUSurface()` | `render/skia_backend.cpp` | Releases FBO, texture, and Skia surface |
| `Engine::UILayer` | `engine/engine.h` | Struct: type (HTML/Canvas) + texture ID + canvas scene ref |
| `Engine::uiLayers_` | `engine/engine.h` | Ordered list of layers built during traversal |
| `Engine::htmlSurfacePool_` | `engine/engine.h` | `vector<SkiaRenderer::GPUSurface>` — reusable GPU surfaces |
| `Engine::compositeLayers()` | `engine/engine.cpp` | GL compositing pass that draws all layers in order |

### Frame lifecycle

```
1. DOM dirty + 8ms elapsed?
   YES → Full rasterization:
         beginFrame()
         rewrap all pool surfaces (fresh Skia wrappers)
         switchSurface(pool[0])           // start HTML layer 0
         drawTraversal.draw()             // walks DOM, fires layer breaks
           → layer break:
             switchSurface(pool[N+1])     // save current as HTML layer, start next
             record canvas layer
         switchSurface(originalSurface)   // save last HTML layer
         flush each pool surface via grContext_->flush(surface)
         endFrame()                       // flushAndSubmit() — textures now ready
   NO  → Skip (HTML layer textures are cached from previous frame)

2. Canvas rasterization (every frame, outside throttle):
   for each canvas layer:
     canvasScene->rasterize()            // flush Ganesh, upload texture

3. Compositing pass (every frame):
   for each layer in uiLayers_:
     HTML   → bind cached texture, draw fullscreen quad
     Canvas → bind canvas texture, draw positioned quad

4. System overlay on top
5. Swap buffers
```

### Surface pooling

HTML layer surfaces are GPU-backed Ganesh surfaces at viewport resolution. They are pooled in `htmlSurfacePool_` and reused between frames. The pool is invalidated on viewport resize (FBOs and textures destroyed and recreated).

### No-canvas apps

When no `<canvas>` elements exist, no layer breaks fire. The traversal draws everything into a single pool surface. The compositing pass draws one fullscreen quad.

### Headless mode

In headless mode, the main loop returns before the GPU frame section. Threading changes must not affect headless. The `LayerBreakCallback` is not set in headless.
