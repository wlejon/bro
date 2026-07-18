# The prebuilt Skia source bundle

Fresh clones do not vendor Skia. `skia.cmake` downloads two things from a GitHub
release on `wlejon/bro`, both pinned to one Skia commit (`chrome/m147`) so the
library always matches the headers:

| Asset | What |
|-------|------|
| `skia-src-m147.tar.gz` | the **source bundle** — the subset of the Skia tree bro compiles and includes |
| `skia-{windows-x64,linux-x64,macos-arm64}-…` | the prebuilt Skia **core** static library |

The core library is only the GN `skia` + `pathops` targets. Every Skia *module*
— `svg`, `skshaper`, `skunicode`, `skresources` — plus HarfBuzz and the ICU bidi
subset is compiled from the source bundle by `skia_modules.cmake`. That is why
enabling text shaping needed no Skia rebuild: the shaping code was never in the
`.lib` to begin with.

## What the bundle must contain

Everything already there (`include/`, `src/`, `modules/`, expat) **plus** the
four trees below. Text shaping (`BRO_WITH_TEXT_SHAPING`, ON in every profile)
does not build without them, and `skia_modules.cmake` hard-fails at configure
time naming the missing files.

| Path (relative to the tarball root) | Why |
|---|---|
| `modules/skunicode/src/` | `SkUnicode.cpp`, `SkUnicode_hardcoded.cpp`, `SkUnicode_bidi.cpp`, `SkUnicode_icu_bidi.cpp`, `SkBidiFactory_icu_subset.cpp`. The pre-shaping bundle shipped this directory's **headers only**. |
| `third_party/externals/harfbuzz/src/` | HarfBuzz. Built as the upstream unity TU `harfbuzz.cc`. |
| `third_party/harfbuzz/` | `config-override.h` (the `std::mutex` shim Skia injects) + `LICENSE`. |
| `third_party/externals/icu/source/common/` | ICU's UAX#9 bidi subset — 14 `.cpp` and their header closure. **Not** full ICU: `source/i18n/`, `source/data/`, and the 30 MB data blob are not needed and must not be shipped. |

`modules/skshaper/src/SkShaper_harfbuzz.cpp` and `SkShaper_skunicode.cpp` are
**already** in the existing bundle — `modules/` is shipped whole. So is the
entire `src/` tree, which is what the three private Skia headers
`SkShaper_harfbuzz.cpp` reaches for (`src/base/SkUTF.h`,
`src/base/SkTDPQueue.h`, `src/core/SkLRUCache.h`) resolve against.

Deliberately **not** vendored: `third_party/libgrapheme` and
`third_party/externals/{libgrapheme,unicodetools}`. Skia uses libgrapheme for
grapheme/line-break iteration, and its tables are generated at build time by six
host C tools run over the Unicode UCD — a codegen step and tens of megabytes of
UCD text. Nothing on bro's shaping path calls a break iterator:
`SkShapers::HB::ShapeDontWrapOrReorder` neither wraps lines nor reorders runs,
so `SkUnicodes::Bidi::Make()` (hardcoded character properties + the ICU bidi
subset) is sufficient. Revisit only if bro ever adopts one of Skia's wrapping
shapers.

## Building and publishing an updated bundle

Run from a **full** Skia checkout at the pinned commit — i.e. a tree that has had
`python3 tools/git-sync-deps` run in it, so `third_party/externals/` is populated.
`third_party/skia/src/` in a developer tree that was built with
`build_skia_linux.sh` / `build_skia_mac.sh` already is one.

```bash
cd third_party/skia/src

tar czf ../skia-src-m147.tar.gz \
  --exclude='*/test/*' --exclude='test-*' --exclude='*/wasm/*' \
  --exclude='*.py' --exclude='*.rl' \
  include \
  src \
  modules \
  third_party/expat third_party/externals/expat \
  third_party/harfbuzz/config-override.h third_party/harfbuzz/LICENSE \
  third_party/externals/harfbuzz/src \
  third_party/externals/icu/source/common third_party/externals/icu/LICENSE

sha256sum ../skia-src-m147.tar.gz
```

Layout convention: paths inside the tarball are relative to the Skia source root
and extract straight into `third_party/skia/src/` (`skia.cmake` extracts with
`DESTINATION third_party/skia/src`). There is no top-level directory in the
archive.

Expected size: the pre-shaping bundle is 6.16 MB; the shaping additions are
2.95 MB compressed (7.2 MB uncompressed), for roughly **9.1 MB**.

### Publish and re-pin

The tarball is served from the same release as the prebuilt libraries. If you
overwrite the asset in place, keep the tag; if you cut a new tag, update
`BRO_SKIA_RELEASE_TAG` too.

```bash
gh release upload skia-prebuilt-m147 third_party/skia/skia-src-m147.tar.gz --clobber
```

Then update `third_party/skia/skia.cmake`:

- **line 32** — `BRO_SKIA_RELEASE_TAG` default, only if you cut a new tag.
- **line 63** — the asset filename, only if you renamed it.
- **line 64** — the SHA-256 literal, **always**: replace it with the
  `sha256sum` printed above.
- **line 62** — the `~6 MB` in the progress message; say `~9 MB`.

Verify from a clean state:

```bash
rm -rf third_party/skia/src            # only if you have no local Skia work
cmake -B build                          # re-downloads and extracts the bundle
cmake --build build --config Release
```

A bundle missing any shaping source makes `cmake -B build` stop with a
`FATAL_ERROR` that names the exact files and the three ways out (republish,
`tools/git-sync-deps`, or `-DBRO_WITH_TEXT_SHAPING=OFF`). It does not silently
fall back to the primitive shaper: once the render layer shapes text, a
primitive-shaper build renders ligatures, kerning and Arabic joining wrong, and
a build that quietly produces incorrect text is worse than one that stops.
