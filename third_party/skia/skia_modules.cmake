# Skia modules — compiled from source against the pre-built Skia core library.
#
# The pre-built skia.lib is only the GN `skia` + `pathops` targets; every Skia
# *module* (svg, skshaper, skresources, skunicode) is absent from it, so we
# compile those here from the source bundle in src/.
#
# Targets built here:
#   skia_svg        modules/svg + modules/skresources + modules/skshaper
#                   (+ modules/skunicode, SkDOM/SkXMLParser, expat) — the target
#                   bro links.
#   skia_harfbuzz   HarfBuzz (unity build)                   [text shaping only]
#   skia_icu_bidi   ICU's UAX#9 bidi subset (14 .cpp)        [text shaping only]
#
# With BRO_WITH_TEXT_SHAPING=ON (the default in every profile) skia_svg also
# carries real HarfBuzz shaping: SkShaper_harfbuzz.cpp + SkShaper_skunicode.cpp
# + the bidi-only SkUnicode (modules/skunicode over the ICU bidi subset). With
# it OFF only the 1:1-codepoint primitive shaper is built.
#
# None of this needs vcpkg, a Skia rebuild, or full ICU — see BUNDLE.md for the
# source-bundle requirements.
#
# Requires the "skia" imported target from skia.cmake.

set(_skia_src "${CMAKE_CURRENT_LIST_DIR}/src")

# ---------------------------------------------------------------------------
# Text shaping: availability check
# ---------------------------------------------------------------------------
# The published source bundle (skia-src-m147.tar.gz) is a subset of the Skia
# tree. Bundles published before text shaping landed carry neither HarfBuzz nor
# the ICU bidi subset nor modules/skunicode's .cpp files. Detect that here and
# fail with an actionable message rather than an avalanche of missing headers.
#
# Hard failure (not a silent fallback to the primitive shaper) is deliberate:
# once the render layer shapes text, a primitive-shaper build silently renders
# ligatures, kerning and Arabic joining *wrong*. A build that quietly produces
# incorrect text is worse than one that stops and names the flag to flip.
if(BRO_WITH_TEXT_SHAPING)
    set(_shaping_missing "")
    foreach(_probe
            "modules/skshaper/src/SkShaper_harfbuzz.cpp"
            "modules/skshaper/src/SkShaper_skunicode.cpp"
            "modules/skunicode/src/SkUnicode.cpp"
            "modules/skunicode/src/SkUnicode_bidi.cpp"
            "modules/skunicode/src/SkBidiFactory_icu_subset.cpp"
            "third_party/harfbuzz/config-override.h"
            "third_party/externals/harfbuzz/src/harfbuzz.cc"
            "third_party/externals/icu/source/common/ubidi.cpp"
            "third_party/externals/icu/source/common/unicode/ubidi.h")
        if(NOT EXISTS "${_skia_src}/${_probe}")
            list(APPEND _shaping_missing "  third_party/skia/src/${_probe}")
        endif()
    endforeach()
    if(_shaping_missing)
        string(REPLACE ";" "\n" _shaping_missing_txt "${_shaping_missing}")
        message(FATAL_ERROR
            "BRO_WITH_TEXT_SHAPING=ON but the Skia source tree is missing the "
            "text-shaping sources:\n${_shaping_missing_txt}\n\n"
            "The prebuilt Skia source bundle for release tag "
            "'${BRO_SKIA_RELEASE_TAG}' predates text shaping (it carries only "
            "include/, src/, the svg/skshaper/skresources modules and expat).\n\n"
            "Fix it in one of these ways:\n"
            "  * Publish (or point at) a bundle that also carries HarfBuzz, the "
            "ICU bidi subset and the modules/skunicode sources — see "
            "third_party/skia/BUNDLE.md for the exact recipe.\n"
            "  * Clone Skia into third_party/skia/src and run "
            "`python3 tools/git-sync-deps` there (fetches the full externals tree).\n"
            "  * Build without shaping: -DBRO_WITH_TEXT_SHAPING=OFF (text falls "
            "back to 1:1 codepoint->glyph mapping: no ligatures, no kerning, no "
            "Arabic joining).")
    endif()
endif()

# ---------------------------------------------------------------------------
# HarfBuzz  (text shaping)
# ---------------------------------------------------------------------------
# harfbuzz.cc is HarfBuzz's own unity build: one TU that #includes every
# implementation file. The platform backends it pulls in (CoreText, DirectWrite,
# FreeType, GDI, Uniscribe, graphite2, glib, wasm) all compile to nothing
# without their HAVE_* defines, so this is exactly Skia's own configuration
# (third_party/harfbuzz/BUILD.gn) minus hb-subset, which shaping never uses.
if(BRO_WITH_TEXT_SHAPING)
    set(_hb_src "${_skia_src}/third_party/externals/harfbuzz/src")

    add_library(skia_harfbuzz STATIC "${_hb_src}/harfbuzz.cc")
    target_include_directories(skia_harfbuzz PUBLIC
        "${_hb_src}"
        "${_skia_src}/third_party/harfbuzz"   # config-override.h
    )
    target_compile_definitions(skia_harfbuzz PUBLIC
        HAVE_OT
        HAVE_CONFIG_OVERRIDE_H
        HB_NO_FALLBACK_SHAPE
        HB_NO_WIN1256
    )
    set_target_properties(skia_harfbuzz PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

    # -----------------------------------------------------------------------
    # ICU bidi subset  (UAX#9 only — 14 .cpp, not the 30 MB ICU blob)
    # -----------------------------------------------------------------------
    # Mirrors Skia's third_party/icu_bidi/BUILD.gn exactly.
    set(_icu_src "${_skia_src}/third_party/externals/icu/source")
    add_library(skia_icu_bidi STATIC
        "${_icu_src}/common/cmemory.cpp"
        "${_icu_src}/common/cstring.cpp"
        "${_icu_src}/common/ubidi.cpp"
        "${_icu_src}/common/ubidi_props.cpp"
        "${_icu_src}/common/ubidiln.cpp"
        "${_icu_src}/common/ubidiwrt.cpp"
        "${_icu_src}/common/uchar.cpp"
        "${_icu_src}/common/udataswp.cpp"
        "${_icu_src}/common/uinvchar.cpp"
        "${_icu_src}/common/ustring.cpp"
        "${_icu_src}/common/ustrtrns.cpp"
        "${_icu_src}/common/utf_impl.cpp"
        "${_icu_src}/common/utrie2.cpp"
        "${_icu_src}/common/utypes.cpp"
    )
    target_include_directories(skia_icu_bidi PUBLIC
        "${_icu_src}/common"
        "${_icu_src}/i18n"
    )
    target_compile_definitions(skia_icu_bidi PUBLIC SK_USING_THIRD_PARTY_ICU)
    target_compile_definitions(skia_icu_bidi PRIVATE
        U_USING_ICU_NAMESPACE=0
        U_COMMON_IMPLEMENTATION
        U_STATIC_IMPLEMENTATION
        U_I18N_IMPLEMENTATION
        # Skia builds this subset with a private symbol suffix so it can never
        # collide with a system ICU. Keep in lockstep with icu_bidi/BUILD.gn.
        U_DISABLE_VERSION_SUFFIX=1
        U_HAVE_LIB_SUFFIX=1
        U_LIB_SUFFIX_C_NAME=_skia
    )
    set_target_properties(skia_icu_bidi PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

    if(MSVC)
        target_compile_options(skia_harfbuzz PRIVATE /W0 /bigobj)
        target_compile_options(skia_icu_bidi PRIVATE /W0)
    else()
        target_compile_options(skia_harfbuzz PRIVATE -w)
        target_compile_options(skia_icu_bidi PRIVATE -w)
    endif()
endif()

# ---------------------------------------------------------------------------
# Skia modules: svg + skresources + skshaper (+ skunicode when shaping)
# ---------------------------------------------------------------------------

file(GLOB _svg_sources      "${_skia_src}/modules/svg/src/*.cpp")
file(GLOB _skresources_src  "${_skia_src}/modules/skresources/src/*.cpp")

set(_skshaper_sources
    "${_skia_src}/modules/skshaper/src/SkShaper.cpp"
    "${_skia_src}/modules/skshaper/src/SkShaper_primitive.cpp"
    "${_skia_src}/modules/skshaper/src/SkShaper_factory.cpp"
)

# modules/skunicode, bidi-only configuration. SkUnicodes::Bidi::Make() pairs the
# hardcoded character-property tables with the ICU bidi subset. That is all
# SkShapers::HB::ShapeDontWrapOrReorder needs — it neither wraps lines nor
# reorders runs, so it never asks SkUnicode for a break iterator. This is why
# libgrapheme is NOT vendored: its tables are generated at build time by six
# host tools run over the Unicode UCD, and nothing on our shaping path uses it.
# (Break iterators would matter only if we adopted one of the wrapping shapers;
# bidi run reordering in chunk 4 uses the bidi factory built here.)
set(_skunicode_sources
    "${_skia_src}/modules/skunicode/src/SkUnicode.cpp"
    "${_skia_src}/modules/skunicode/src/SkUnicode_hardcoded.cpp"
    "${_skia_src}/modules/skunicode/src/SkUnicode_bidi.cpp"
    "${_skia_src}/modules/skunicode/src/SkUnicode_icu_bidi.cpp"
    "${_skia_src}/modules/skunicode/src/SkBidiFactory_icu_subset.cpp"
)

if(BRO_WITH_TEXT_SHAPING)
    list(APPEND _skshaper_sources
        "${_skia_src}/modules/skshaper/src/SkShaper_harfbuzz.cpp"
        "${_skia_src}/modules/skshaper/src/SkShaper_skunicode.cpp"
    )
else()
    set(_skunicode_sources "")
endif()

# Skia's XML/DOM parser (SkDOM + SkXMLParser) and expat — these may not be
# included in the pre-built skia.lib (e.g. Windows builds without system expat).
# Compiling them here is harmless if they're already in skia.lib (linker dedupes).
set(_xml_sources
    "${_skia_src}/src/xml/SkDOM.cpp"
    "${_skia_src}/src/xml/SkXMLParser.cpp"
    "${_skia_src}/src/xml/SkXMLWriter.cpp"
)
set(_expat_sources
    "${_skia_src}/third_party/externals/expat/expat/lib/xmlparse.c"
    "${_skia_src}/third_party/externals/expat/expat/lib/xmlrole.c"
    "${_skia_src}/third_party/externals/expat/expat/lib/xmltok.c"
)

add_library(skia_svg STATIC
    ${_svg_sources}
    ${_skresources_src}
    ${_skshaper_sources}
    ${_skunicode_sources}
    ${_xml_sources}
    ${_expat_sources}
)

# --- Include paths ---
# Skia source root (for include/core/..., src/..., modules/.../include/...)
target_include_directories(skia_svg PRIVATE "${_skia_src}")

# Module public headers — consumers of skia_svg get these too
target_include_directories(skia_svg PUBLIC
    "${_skia_src}/modules/svg/include"
    "${_skia_src}/modules/skshaper/include"
    "${_skia_src}/modules/skresources/include"
    "${_skia_src}/modules/skunicode/include"
    "${_skia_src}/modules/skshaper/utils"
)

# Expat headers (bundled in Skia's third_party)
target_include_directories(skia_svg PRIVATE
    "${_skia_src}/third_party/externals/expat/expat/lib"
    "${_skia_src}/third_party/expat/include/expat_config"
)

# --- Defines ---
target_compile_definitions(skia_svg PUBLIC
    SK_ENABLE_SVG
    SK_SHAPER_PRIMITIVE_AVAILABLE
)
target_compile_definitions(skia_svg PRIVATE
    XML_STATIC
    SKSHAPER_IMPLEMENTATION=1
    SKUNICODE_IMPLEMENTATION=1
)

if(BRO_WITH_TEXT_SHAPING)
    # PUBLIC: SkShapers::BestAvailable() is a header-only inline that switches on
    # these, so every consumer must see them (FactoryHelpers.h says so verbatim).
    target_compile_definitions(skia_svg PUBLIC
        SK_SHAPER_HARFBUZZ_AVAILABLE
        SK_SHAPER_UNICODE_AVAILABLE
        SK_UNICODE_AVAILABLE
        SK_UNICODE_BIDI_IMPLEMENTATION
        BRO_WITH_TEXT_SHAPING=1
    )
    # ICU symbol-renaming macros. These are what turn `ubidi_setPara` into
    # `ubidi_setPara_skia` *in the ICU headers*, so the skunicode sources must
    # compile with the exact same set the bidi subset was built with or the
    # calls resolve to names that do not exist. Copied verbatim from Skia's
    # modules/skunicode/BUILD.gn bidi_subset target.
    target_compile_definitions(skia_svg PRIVATE
        U_DISABLE_RENAMING=0
        U_USING_ICU_NAMESPACE=0
        U_LIB_SUFFIX_C_NAME=_skia
        U_HAVE_LIB_SUFFIX=1
        U_DISABLE_VERSION_SUFFIX=1
    )
    target_link_libraries(skia_svg PRIVATE skia_harfbuzz skia_icu_bidi)
else()
    target_compile_definitions(skia_svg PUBLIC BRO_WITH_TEXT_SHAPING=0)
endif()

# --- Link pre-built Skia core ---
target_link_libraries(skia_svg PUBLIC skia)

# MSVC: disable noisy warnings from Skia source
if(MSVC)
    target_compile_options(skia_svg PRIVATE /W0)
else()
    target_compile_options(skia_svg PRIVATE -w)
endif()
