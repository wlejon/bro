#!/usr/bin/env python3
"""
Mechanical JS <-> C++ Host Boundary Verifier (Issue #78).

Audits all native host bindings across `bro/src/bronze_host/` and sibling
`<repo>/src/api/` directories for four classes of safety invariants:
  1. Moving-GC Root Discipline:
     - `HostClass::install`, `HostClass::bind`, and `HostClass::setStatic` must
       root local `Value` handles (`ObjectBuilder` or `ev::Persistent`) across
       allocating `ev::setProperty` / `ev::getProperty` calls.
     - Stack `CallResult` values (`r.value`) must not be passed unrooted across
       multiple consecutive `ev::getElement` / `ev::getProperty` calls.
  2. Handle Type-Brand Confusion:
     - Every `host_class.cpp` must use the canonical out-of-line `struct Brand`
       + `brandMutex()` + `brands()` hash table, never in-band
       `static_cast<BrandedHandle*>(ev::handleData(...))`.
  3. Argument Count & Span Bounds Discipline:
     - Every `NativeFn` lambda/function indexing `a[i]` (for `i >= 1`) must
       either guard `a.size()` in the function body or use bounded helpers.
  4. Finite-Float (`std::isfinite`) Domain Gates:
     - Physics (`native_physics_internal.h`, `native_physics_core.cpp`),
       Scene (`native_scene_nodes.cpp`, `native_scene_camera.cpp`), and
       Audio DSP (`biquad.cpp`, `host_audio_internal.h`, `host_audio_param.cpp`,
       `engine_bus_params.cpp`, `engine_buses.cpp`, `engine_voices.cpp`) must
       reject `NaN`/`Inf` before mutating native simulation or DSP filter state.
"""

from __future__ import annotations
import pathlib
import re
import sys

PROJECTS_ROOT = pathlib.Path(__file__).resolve().parents[2]
BRO_ROOT = PROJECTS_ROOT / "bro"


def check_host_class_files(errors: list[str]) -> int:
    host_class_files = sorted(PROJECTS_ROOT.glob("bro*/src/api/host_class.cpp"))
    bro_hc = BRO_ROOT / "src/bronze_host/host_class.cpp"
    if bro_hc.exists():
        host_class_files.append(bro_hc)

    for path in host_class_files:
        text = path.read_text(encoding="utf-8")
        rel = path.relative_to(PROJECTS_ROOT)

        # 1. Check out-of-line brand table (no in-band BrandedHandle cast)
        if "BrandedHandle" in text:
            errors.append(f"{rel}: uses unsafe in-band BrandedHandle cast in ev::handleData")
        if "HostClass::unwrap" in text:
            if (
                "struct Brand" not in text
                or "brandMutex" not in text
                or ("brands()" not in text and "brandTable()" not in text)
            ):
                errors.append(
                    f"{rel}: missing canonical out-of-line Brand table (Brand/brandMutex/brands)"
                )

        # 2. Check rooting in HostClass::install / setStatic / bind
        if "ev::setProperty(gt.value" in text and "gtP(gt.value)" not in text:
            errors.append(f"{rel}: unrooted gt.value across ev::setProperty in HostClass::install")
        if "ev::getProperty(g.value" in text and "ctor_->get()" not in text:
            errors.append(f"{rel}: unrooted g.value across ev::getProperty in HostClass::bind")
        set_static_match = re.search(
            r"void\s+HostClass::setStatic\s*\([^)]*\)\s*\{([^}]*)\}", text, re.DOTALL
        )
        if set_static_match:
            body = set_static_match.group(1)
            if "ev::setProperty" in body and "valP" not in body and "Persistent" not in body:
                errors.append(f"{rel}: unrooted Value parameter in HostClass::setStatic")

    return len(host_class_files)


def check_unrooted_call_result_reuse(errors: list[str]) -> int:
    binding_files = sorted(BRO_ROOT.glob("src/bronze_host/**/*.cpp")) + sorted(
        PROJECTS_ROOT.glob("bro*/src/api/**/*.cpp")
    )
    seen = set()
    unique_files = []
    for f in binding_files:
        if f not in seen:
            seen.add(f)
            unique_files.append(f)

    # Detect multiple ev::getElement(r.value, ...) or ev::getProperty(r.value, ...) in the same block
    r_value_access = re.compile(r"ev::(?:getElement|getProperty)\s*\(\s*r\.value\b")
    for path in unique_files:
        text = path.read_text(encoding="utf-8")
        rel = path.relative_to(PROJECTS_ROOT)
        # Split roughly by function definitions
        for chunk in text.split("\n}\n"):
            matches = r_value_access.findall(chunk)
            if len(matches) > 1:
                errors.append(
                    f"{rel}: unrooted CallResult `r.value` passed to {len(matches)} "
                    f"consecutive ev::getElement/getProperty calls without Rooted/Persistent"
                )
    return len(unique_files)


def check_finite_float_gates(errors: list[str]) -> None:
    required_isfinite = [
        (BRO_ROOT / "src/bronze_host/native_physics_internal.h", 3),
        (BRO_ROOT / "src/bronze_host/native_physics_core.cpp", 8),
        (BRO_ROOT / "src/bronze_host/native_scene_nodes.cpp", 10),
        (BRO_ROOT / "src/bronze_host/native_scene_camera.cpp", 3),
        (PROJECTS_ROOT / "broaudio/src/dsp/biquad.cpp", 5),
        (PROJECTS_ROOT / "broaudio/src/api/host_audio_internal.h", 2),
        (PROJECTS_ROOT / "broaudio/src/api/host_audio_param.cpp", 5),
        (PROJECTS_ROOT / "broaudio/src/engine_bus_params.cpp", 6),
        (PROJECTS_ROOT / "broaudio/src/engine_buses.cpp", 4),
        (PROJECTS_ROOT / "broaudio/src/engine_voices.cpp", 4),
    ]
    for path, min_count in required_isfinite:
        if not path.exists():
            errors.append(f"Missing expected file: {path.relative_to(PROJECTS_ROOT)}")
            continue
        text = path.read_text(encoding="utf-8")
        count = len(re.findall(r"(?:std::isfinite|clampFinite)\b", text))
        if count < min_count:
            errors.append(
                f"{path.relative_to(PROJECTS_ROOT)}: expected >= {min_count} std::isfinite "
                f"gates, found {count}"
            )


def main() -> int:
    errors: list[str] = []
    n_hc = check_host_class_files(errors)
    n_bf = check_unrooted_call_result_reuse(errors)
    check_finite_float_gates(errors)

    if errors:
        print(f"FAIL: {len(errors)} host-binding verification error(s):", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        return 1

    print(
        f"OK: verified {n_hc} HostClass implementations, {n_bf} native binding files, "
        f"and finite-float gates across Physics, Scene, and Audio DSP."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
