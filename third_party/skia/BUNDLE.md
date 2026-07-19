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
| `third_party/externals/libwebp/src/{dec,dsp,utils,webp}/` + `enc/*.h` | libwebp's decoder (`BRO_WITH_WEBP`, ON in every profile). Decode only — `enc/`, `mux/` and `demux/` sources are not shipped. The enc **headers** are, because `dsp/lossless.h` includes `enc/histogram_enc.h`; without them `dsp/lossless.c` will not compile. |

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

**Build the new bundle from the previous published one, not from a full Skia
checkout.** The published tarball is trimmed more aggressively than any short
list of `--exclude` globs reproduces — re-deriving it from a `git-sync-deps`
tree with the obvious excludes pulls in the whole of `modules/` (canvaskit's
npm/go/wasm trees, skottie sources, per-module test dirs) and yields a **37 MB**
archive instead of 9 MB. Starting from the last good bundle preserves the exact
trim set and guarantees the parts that already build keep building.

```bash
cd "$(mktemp -d)"
curl -sL -o old.tar.gz \
  https://github.com/wlejon/bro/releases/download/skia-prebuilt-m147/skia-src-m147.tar.gz

mkdir staging && tar xzf old.tar.gz -C staging

SRC=/path/to/full/skia            # a tree with tools/git-sync-deps run in it
mkdir -p staging/modules/skunicode/src staging/third_party/harfbuzz \
         staging/third_party/externals/harfbuzz staging/third_party/externals/icu/source
cp -r "$SRC/modules/skunicode/src/."                    staging/modules/skunicode/src/
cp    "$SRC/third_party/harfbuzz/config-override.h" \
      "$SRC/third_party/harfbuzz/LICENSE"               staging/third_party/harfbuzz/
cp -r "$SRC/third_party/externals/harfbuzz/src"         staging/third_party/externals/harfbuzz/
cp -r "$SRC/third_party/externals/icu/source/common"    staging/third_party/externals/icu/source/
cp    "$SRC/third_party/externals/icu/LICENSE"          staging/third_party/externals/icu/

WEBP="$SRC/third_party/externals/libwebp"
mkdir -p staging/third_party/externals/libwebp/src/enc
cp -r "$WEBP"/src/{dec,dsp,utils,webp} staging/third_party/externals/libwebp/src/
cp    "$WEBP"/src/enc/*.h              staging/third_party/externals/libwebp/src/enc/
cp    "$WEBP"/{COPYING,PATENTS,AUTHORS} staging/third_party/externals/libwebp/

find staging -type d \( -name test -o -name tests -o -name wasm \) -prune -exec rm -rf {} +
find staging -name '*.py' -delete
find staging -name '*.rl' -delete

(cd staging && tar czf ../skia-src-m147.tar.gz include src modules third_party)
sha256sum skia-src-m147.tar.gz
```

Layout convention: paths inside the tarball are relative to the Skia source root
and extract straight into `third_party/skia/src/` (`skia.cmake` extracts with
`DESTINATION third_party/skia/src`). There is no top-level directory in the
archive.

Sizes: pre-shaping bundle 6,159,567 B; with shaping 9,132,917 B; with shaping +
WebP **9,513,711 B** (~9.5 MB).

### `third_party/skia/src/` is gitignored — do not trust it

The single most likely way to break CI here is to add a `skia_modules.cmake`
target that compiles files your **local** `src/` happens to have. If you ever
hand-built Skia, `src/` is a full `git-sync-deps` checkout carrying all ~40
externals, so a new target builds clean locally and then fails at *configure*
on every machine that fetched the published bundle — which ships four.

This is exactly how WebP shipped broken: `libwebp` was present locally, absent
from the bundle, and the whole feature (build, full suite, commit) was
validated without ever exercising the path CI takes. Before publishing a target
that reaches into `third_party/externals/`, either extract the bundle somewhere
empty and compile the source list against *that*, or confirm the tree is in the
table above.

### Verify before publishing

Extract the archive somewhere empty and confirm every file
`skia_modules.cmake` probes for is present, plus the three private Skia headers
`SkShaper_harfbuzz.cpp` reaches for (`src/base/SkUTF.h`, `src/base/SkTDPQueue.h`,
`src/core/SkLRUCache.h`) and `include/core/SkCanvas.h` (the "already fetched"
sentinel `skia.cmake` tests). A bundle that extracts with a stray top-level
directory will pass a naive `tar tzf | grep` but fail at configure.

### Publish and re-pin

The tarball is served from the same release as the prebuilt libraries.
Overwriting the asset in place is fine — bro's CI is the only consumer, and it
builds from this repo, so it re-pins in the same commit.

```bash
gh release upload skia-prebuilt-m147 skia-src-m147.tar.gz --repo wlejon/bro --clobber
```

Then update `third_party/skia/skia.cmake` **in the same commit as the upload** —
the checksum is verified on download, so a published asset and an un-updated pin
break every fresh configure until they agree:

- `BRO_SKIA_RELEASE_TAG` default — only if you cut a new tag.
- the asset filename in the `_bro_skia_download` call — only if you renamed it.
- the SHA-256 literal — **always**, from the `sha256sum` above.
- the `~9 MB` in the progress message, if the size moved materially.

Finally re-download from the release URL and confirm the checksum matches what
you pinned, so a truncated or mid-flight upload can't sit undetected.

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
