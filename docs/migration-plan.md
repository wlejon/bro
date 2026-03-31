# Migration Plan: litehtml → htmlayout

## Overview

Replace litehtml (HTML/CSS parser + layout engine) with htmlayout (standalone CSS + layout library at D:\projects\htmlayout). htmlayout provides CSS parsing, selector matching with shadow DOM scoping, style cascade, and block/inline/flex layout.

## What Changes

### Goes Away (litehtml)
- `third_party/litehtml/` submodule
- `litehtml::document` — HTML parsing + CSS + layout + draw orchestration
- `litehtml::element::ptr` — stored on every `bro::dom::Element`
- `litehtml::html_tag` — base class for replaced elements
- `litehtml::render_item` — layout boxes, hit testing, scrolling
- `BroContainer` (`src/layout/container.h/cpp`) — litehtml's document_container bridge
- `BroElement` (`src/layout/bro_element.h/cpp`) — custom litehtml element subclass
- `BroElText` (`src/layout/bro_el_text.h`) — custom text element
- Pointer-to-member hacks for accessing litehtml internals
- `purgeExpiredRenders()` — litehtml memory workaround

### Stays
- gumbo HTML parser (now bundled directly, or shared from htmlayout)
- `bro::dom::*` — Element, Document, Node, TextNode, Event, ShadowRoot, StyleProxy
- `bro::js::*` — all JS bindings, custom elements, event dispatch
- `bro::render::*` — Skia renderer, GPU context
- `bro::platform::*` — SDL3 window, event loop
- `bro::engine::*` — main loop, app loading (modified to use htmlayout)
- `bro::canvas::*`, `bro::webgl::*`, `bro::audio::*`, `bro::svg::*`

### New
- `ElementRefAdapter` — implements `htmlayout::css::ElementRef` wrapping `bro::dom::Element`
- `LayoutNodeAdapter` — implements `htmlayout::layout::LayoutNode` wrapping `bro::dom::Element`
- `SkiaTextMetrics` — implements `htmlayout::layout::TextMetrics` using Skia font measurement
- Draw traversal — walks htmlayout's LayoutBox tree and issues Skia draw calls (replaces litehtml's draw callbacks via BroContainer)

## File-by-File Migration

### 1. `src/dom/element.h` / `.cpp`
**Remove**: `litehtml::element::ptr litehtml_element_` member, `setLitehtmlElement()`, `litehtmlElement()`, `syncStylesToLitehtml()`
**Add**: `htmlayout::layout::LayoutBox layoutBox_` member, `htmlayout::css::ComputedStyle computedStyle_`
**Keep**: Everything else (tag, attributes, style proxy, children, listeners, shadow root, dirty tracking)

### 2. `src/dom/document.h` / `.cpp`
**Remove**: `litehtml::document::ptr litehtml_doc_`, `litehtmlMap_`, all `syncAppendToLitehtml`/`syncRemoveFromLitehtml`/`syncInsertBeforeLitehtml`, `linkElementToLitehtml`, `unlinkLitehtmlRecursive`, litehtml-based `parse()`/`buildFrom()`
**Add**: `htmlayout::css::Cascade cascade_`, `parseWithGumbo()` (use gumbo directly), `resolveStyles()`, `performLayout()`
**Keep**: `createElement`, `freeNode`, `getElementById`, `querySelector`/`querySelectorAll` (rewrite to use htmlayout selectors or keep simple matching)

### 3. `src/layout/container.h` / `.cpp`
**Remove entirely**. The BroContainer implemented litehtml's `document_container` interface (draw callbacks). Replaced by a new draw traversal that walks LayoutBox tree directly.

### 4. `src/layout/bro_element.h` / `.cpp`
**Remove entirely**. Custom litehtml element subclass no longer needed.

### 5. `src/layout/bro_el_text.h`
**Remove entirely**. Custom litehtml text element no longer needed.

### 6. `src/layout/el_input.h/cpp`, `el_textarea.h/cpp`, `el_select.h/cpp`
**Major rewrite**. These currently extend `litehtml::html_tag` as replaced elements. They need to become standalone renderers that:
- Read computed style from htmlayout
- Draw themselves using the Renderer interface
- Handle input/interaction independently

### 7. `src/layout/el_svg.h/cpp`
**Rewrite**. Currently extends litehtml element. Needs to become a standalone SVG renderer reading layout box position.

### 8. `src/engine/engine.h` / `.cpp`
**Major rewrite of render loop**:
- Remove: `litehtmlDoc_`, `rebuild_render_tree()`, `litehtmlDoc_->render()`, `litehtmlDoc_->draw()`
- Add: `cascade_.resolve()` for each element, `htmlayout::layout::layoutTree()`, custom draw traversal
- Keep: dirty tracking, SDL event handling, GPU pipeline

### 9. `src/engine/system_overlay.h` / `.cpp`
**Rewrite overlay rendering** to use htmlayout instead of separate litehtml document.

### 10. `src/headless/headless.h` / `.cpp`
**Rewrite** to use htmlayout for layout/rendering. The headless mode uses a RasterRenderer — it needs layout boxes to render into.

### 11. `src/js/dom_bindings.cpp`
**Remove**: All litehtml references in syncAppendToLitehtml calls, litehtml-based querySelector/querySelectorAll fallbacks
**Keep**: All element/document/event JS class definitions, wrapper functions

### 12. `src/svg/svg_parser.h` / `.cpp`
**Minor change**: Remove litehtml includes, use standalone SVG element handling.

## Draw Traversal (replaces BroContainer)

The new draw system walks the LayoutBox tree and issues Skia calls directly:

```
for each element in tree (depth-first):
    read element.layoutBox (position, size, margin, padding, border)
    read element.computedStyle

    draw background (background-color, background-image, gradients)
    draw borders (border-width, border-style, border-color)
    draw text (for text nodes)
    clip children (for overflow: hidden/scroll/auto)
    recurse into children
```

This replaces the ~600 LOC BroContainer with a simpler traversal since htmlayout provides all the geometry.

## Migration Order

1. **Add htmlayout as dependency** — add to third_party or as sibling project link
2. **Create adapters** — ElementRefAdapter, LayoutNodeAdapter, SkiaTextMetrics
3. **Migrate Document** — replace litehtml parsing with gumbo + htmlayout cascade
4. **Migrate Element** — remove litehtml::element::ptr, add LayoutBox + ComputedStyle
5. **Build draw traversal** — replace BroContainer with direct LayoutBox → Skia rendering
6. **Migrate Engine render loop** — use htmlayout for layout, new draw traversal for rendering
7. **Migrate replaced elements** — el_input, el_textarea, el_select as standalone renderers
8. **Migrate headless** — same changes for headless renderer
9. **Remove litehtml** — delete submodule, clean CMakeLists
10. **Test everything** — all 142 jQuery tests + shadow DOM tests + visual verification
