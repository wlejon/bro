#!/usr/bin/env python3
"""
Mechanical JS <-> C++ Host Boundary Verifier (Issues #78 & #84).

Audits all native host bindings across `bro/src/bronze_host/` and all 28 sibling
`<repo>/src/api/` directories for four classes of safety invariants:
  1. Moving-GC Root Discipline:
     - `HostClass::install`, `HostClass::bind`, and `HostClass::setStatic` must
       root local `Value` handles (`ObjectBuilder` or `ev::Persistent`) across
       allocating `ev::setProperty` / `ev::getProperty` calls.
     - Stack `CallResult` values (`r.value`) and unrooted `Value` locals must
       not survive across allocating `ev::*` calls without `Rooted` /
       `ev::Persistent` / `ObjectBuilder`.
  2. Handle Type-Brand Confusion:
     - Every `host_class.cpp` must use the canonical out-of-line `struct Brand`
       + `brandMutex()` + `brands()` hash table, never in-band
       `static_cast<BrandedHandle*>(ev::handleData(...))`.
  3. Argument Count & Span Bounds Discipline:
     - Every `NativeFn` lambda/function indexing `a[i]` / `args[i]` must guard
       `a.size()` / `args.size()` or use bounded helpers before indexing.
  4. Finite-Float (`std::isfinite`) Domain Gates:
     - Physics, Scene, Audio DSP, and all 3D / Flora / GameAgent / Tensor / ML /
       Compositor / Display `<sibling>_api` repositories (`bromesh`, `broflora`,
       `brogameagent`, `brotensor`, `brolm`, `brodiffusion`, `brosoundml`,
       `brovisionml`, `brocompositor`, `brodisplays`) must guard numeric/float
       conversions with `std::isfinite` / `finiteFloat` / `clampFinite`.
"""

from __future__ import annotations
import pathlib
import re
import sys

PROJECTS_ROOT = pathlib.Path(__file__).resolve().parents[2]
BRO_ROOT = PROJECTS_ROOT / "bro"

ALLOC_RE = re.compile(
    r"\b(?:ev|bronze::embed)::(?:"
    r"createObject|createArray|createFunction|createHostFunction|createClass|"
    r"createTypedArray|createArrayBuffer|fromUtf8|setProperty|setElement|"
    r"getProperty|getElement|call|callMethod|construct|parseJson|newError|"
    r"eval)\s*\("
)

STR_LITERAL_RE = re.compile(r'"(?:\\.|[^"\\])*"')

VAL_DECL_RE = re.compile(
    r"^\s*(?:const\s+)?(?:bronze::)?Value\s+([a-zA-Z_][a-zA-Z0-9_]*)\s*=\s*(.+);"
)

IMM_RHS_RE = re.compile(
    r"\b(?:fromDouble|fromBool|undefined|null|trueValue|falseValue)\s*\("
)


def collect_binding_files() -> list[pathlib.Path]:
    files = (
        sorted(BRO_ROOT.glob("src/bronze_host/**/*.cpp"))
        + sorted(BRO_ROOT.glob("src/bronze_host/**/*.h"))
        + sorted(PROJECTS_ROOT.glob("bro*/src/api/**/*.cpp"))
        + sorted(PROJECTS_ROOT.glob("bro*/src/api/**/*.h"))
    )
    seen: set[pathlib.Path] = set()
    unique: list[pathlib.Path] = []
    for f in files:
        if f not in seen:
            seen.add(f)
            unique.append(f)
    return unique


def check_host_class_files(errors: list[str]) -> int:
    host_class_files = sorted(PROJECTS_ROOT.glob("bro*/src/api/host_class.cpp"))
    bro_hc = BRO_ROOT / "src/bronze_host/host_class.cpp"
    if bro_hc.exists() and bro_hc not in host_class_files:
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


def check_unrooted_values(unique_files: list[pathlib.Path], errors: list[str]) -> None:
    r_value_access = re.compile(r"ev::(?:getElement|getProperty)\s*\(\s*r\.value\b")
    for path in unique_files:
        text = path.read_text(encoding="utf-8")
        rel = path.relative_to(PROJECTS_ROOT)

        # 1. Check unrooted CallResult r.value reuse
        for chunk in text.split("\n}\n"):
            matches = r_value_access.findall(chunk)
            if len(matches) > 1:
                errors.append(
                    f"{rel}: unrooted CallResult `r.value` passed to {len(matches)} "
                    f"consecutive ev::getElement/getProperty calls without Rooted/Persistent"
                )

        # 2. Check unrooted Value locals held across intermediate allocating ev::* calls
        lines = text.splitlines()
        for idx, line in enumerate(lines):
            m = VAL_DECL_RE.match(line)
            if not m:
                continue
            vname, rhs = m.group(1), m.group(2)
            if IMM_RHS_RE.search(rhs):
                continue
            v_re = re.compile(r"(?<![.\w])" + re.escape(vname) + r"\b")
            assign_re = re.compile(r"(?<![.\w])" + re.escape(vname) + r"\s*=")
            alloc_lines: list[tuple[int, int]] = []
            depth = 0
            for j in range(idx + 1, min(idx + 25, len(lines))):
                lj = STR_LITERAL_RE.sub('""', lines[j].split("//")[0])
                if "};" in lj or re.match(r"^\}\s*$", lines[j]):
                    break
                depth += lj.count("{") - lj.count("}")
                if depth < 0:
                    break
                if re.search(r"\b(?:return|continue|break)\b", lj):
                    if depth == 0:
                        break
                    alloc_lines = [(ln, d) for ln, d in alloc_lines if d < depth]
                    continue
                if assign_re.search(lj):
                    alloc_lines.clear()
                    continue
                if v_re.search(lj) and alloc_lines:
                    # Ignore immediate checks (isNumber/toDouble) on numbers or continuation of ev::call
                    if re.search(
                        r"\b(?:isNumber|toDouble|isBool|toBool|isUndefined|isNull|finiteFloat|finiteDouble)\s*\(\s*"
                        + re.escape(vname)
                        + r"\b",
                        lj,
                    ):
                        continue
                    if alloc_lines[-1][0] == j and "std::span<const Value>" in lj:
                        continue
                    errors.append(
                        f"{rel}:{idx+1}: unrooted Value `{vname}` used at line {j+1} "
                        f"after allocating call at line {alloc_lines[0][0]}"
                    )
                    break
                if ALLOC_RE.search(lj):
                    alloc_lines.append((j + 1, depth))


def check_span_arg_bounds(unique_files: list[pathlib.Path], errors: list[str]) -> int:
    span_param_re = re.compile(
        r"std::span<\s*const\s+(?:bronze::)?Value\s*>\s+([a-zA-Z_][a-zA-Z0-9_]*)"
    )
    checked_spans = 0
    for path in unique_files:
        text = path.read_text(encoding="utf-8")
        rel = path.relative_to(PROJECTS_ROOT)
        lines = text.splitlines()
        for idx, line in enumerate(lines):
            m = span_param_re.search(line)
            if not m:
                continue
            checked_spans += 1
            aname = m.group(1)
            idx_re = re.compile(r"\b" + re.escape(aname) + r"\s*\[\s*(\d+)\s*\]")
            guard_re = re.compile(
                r"\b"
                + re.escape(aname)
                + r"\.(?:size|empty)\b"
                + r"|\b(?:hasArg|argAt|argOr|isObjectArg|isStringArg|isNumberArg|"
                + r"countArg|intArg|optIntArg|seedArg|nodeArg|fieldDims|"
                + r"readPcmArg|readWindow|excludeTagAt|parseAlgorithm)\s*\("
            )
            guarded = False
            depth = 0
            started = False
            for j in range(idx, min(idx + 80, len(lines))):
                lj = lines[j].split("//")[0]
                if "{" in lj:
                    started = True
                depth += lj.count("{") - lj.count("}")
                if guard_re.search(lj):
                    guarded = True
                for im in idx_re.finditer(lj):
                    if not guarded:
                        errors.append(
                            f"{rel}:{j+1}: unchecked `{aname}[{im.group(1)}]` before "
                            f"`{aname}.size()` / `{aname}.empty()` guard"
                        )
                if started and depth <= 0:
                    break
    return checked_spans


def check_finite_float_gates(errors: list[str]) -> int:
    required_isfinite = [
        # bro engine (Physics & Scene)
        (BRO_ROOT / "src/bronze_host/native_physics_internal.h", 3),
        (BRO_ROOT / "src/bronze_host/native_physics_core.cpp", 8),
        (BRO_ROOT / "src/bronze_host/native_scene_nodes.cpp", 10),
        (BRO_ROOT / "src/bronze_host/native_scene_camera.cpp", 3),
        (BRO_ROOT / "src/bronze_host/host_sibling_apis.cpp", 2),
        # broaudio DSP & API
        (PROJECTS_ROOT / "broaudio/src/dsp/biquad.cpp", 5),
        (PROJECTS_ROOT / "broaudio/src/api/host_audio_internal.h", 2),
        (PROJECTS_ROOT / "broaudio/src/api/host_audio_param.cpp", 5),
        (PROJECTS_ROOT / "broaudio/src/engine_bus_params.cpp", 6),
        (PROJECTS_ROOT / "broaudio/src/engine_buses.cpp", 4),
        (PROJECTS_ROOT / "broaudio/src/engine_voices.cpp", 4),
        # bromesh 3D mesh / SDF / plant / rigging
        (PROJECTS_ROOT / "bromesh/src/api/arg_reader.h", 3),
        (PROJECTS_ROOT / "bromesh/src/api/native_mesh_plants.cpp", 12),
        (PROJECTS_ROOT / "bromesh/src/api/native_mesh_sdf.cpp", 8),
        (PROJECTS_ROOT / "bromesh/src/api/native_mesh_analysis.cpp", 4),
        (PROJECTS_ROOT / "bromesh/src/api/native_mesh_io.cpp", 4),
        (PROJECTS_ROOT / "bromesh/src/api/native_mesh_ops.cpp", 2),
        (PROJECTS_ROOT / "bromesh/src/api/native_rigging_anim.cpp", 4),
        (PROJECTS_ROOT / "bromesh/src/api/native_rigging_core.cpp", 1),
        # broflora botanical simulation
        (PROJECTS_ROOT / "broflora/src/api/native_flora_internal.h", 6),
        (PROJECTS_ROOT / "broflora/src/api/native_flora.cpp", 14),
        (PROJECTS_ROOT / "broflora/src/api/native_flora_herbs.cpp", 1),
        # brogameagent AI / navigation / MCTS
        (PROJECTS_ROOT / "brogameagent/src/api/arg_reader.h", 2),
        (PROJECTS_ROOT / "brogameagent/src/api/host_ai_internal.h", 3),
        (PROJECTS_ROOT / "brogameagent/src/api/host_ai_mcts.cpp", 1),
        (PROJECTS_ROOT / "brogameagent/src/api/host_ai_mcts_support.cpp", 3),
        # brotensor GPU tensor & safetensors
        (PROJECTS_ROOT / "brotensor/src/api/native_tensor_nn.cpp", 1),
        (PROJECTS_ROOT / "brotensor/src/api/native_tensor_safetensors.cpp", 2),
        # brolm LLM / Laya
        (PROJECTS_ROOT / "brolm/src/api/arg_reader.h", 1),
        (PROJECTS_ROOT / "brolm/src/api/host_lm_internal.h", 1),
        (PROJECTS_ROOT / "brolm/src/api/native_lm_laya.cpp", 1),
        # brodiffusion image/3D diffusion
        (PROJECTS_ROOT / "brodiffusion/src/api/arg_reader.h", 3),
        (PROJECTS_ROOT / "brodiffusion/src/api/native_diffusion_control.cpp", 1),
        (PROJECTS_ROOT / "brodiffusion/src/api/native_diffusion_qwenimage21_sched.cpp", 1),
        (PROJECTS_ROOT / "brodiffusion/src/api/native_diffusion_terrain.cpp", 2),
        # brosoundml speech / music / wake
        (PROJECTS_ROOT / "brosoundml/src/api/host_soundml_internal.h", 5),
        (PROJECTS_ROOT / "brosoundml/src/api/native_soundml_tts_qwen.cpp", 1),
        (PROJECTS_ROOT / "brosoundml/src/api/native_soundml_wake.cpp", 1),
        # brovisionml computer vision
        (PROJECTS_ROOT / "brovisionml/src/api/arg_reader.h", 1),
        (PROJECTS_ROOT / "brovisionml/src/api/host_vision_internal.h", 4),
        (PROJECTS_ROOT / "brovisionml/src/api/native_vision_models.cpp", 1),
        (PROJECTS_ROOT / "brovisionml/src/api/native_vision_ops.cpp", 3),
        (PROJECTS_ROOT / "brovisionml/src/api/native_vision_sam2.cpp", 2),
        # brocompositor & brodisplays
        (PROJECTS_ROOT / "brocompositor/src/api/arg_reader.h", 2),
        (PROJECTS_ROOT / "brocompositor/src/api/native_interaction.cpp", 2),
        (PROJECTS_ROOT / "brodisplays/src/api/arg_reader.h", 2),
    ]
    gate_re = re.compile(
        r"\b(?:std::isfinite|finiteFloat|finiteDouble|clampFinite|readFiniteFloatProp|readFiniteFloatElem)\b"
    )
    for path, min_count in required_isfinite:
        if not path.exists():
            errors.append(f"Missing expected file: {path.relative_to(PROJECTS_ROOT)}")
            continue
        text = path.read_text(encoding="utf-8")
        count = len(gate_re.findall(text))
        if count < min_count:
            errors.append(
                f"{path.relative_to(PROJECTS_ROOT)}: expected >= {min_count} finite-float "
                f"gates, found {count}"
            )
    return len(required_isfinite)


def main() -> int:
    errors: list[str] = []
    unique_files = collect_binding_files()
    n_hc = check_host_class_files(errors)
    check_unrooted_values(unique_files, errors)
    n_spans = check_span_arg_bounds(unique_files, errors)
    n_fg = check_finite_float_gates(errors)

    if errors:
        print(f"FAIL: {len(errors)} host-binding verification error(s):", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        return 1

    print(
        f"OK: verified {n_hc} HostClass implementations, {len(unique_files)} native binding files, "
        f"{n_spans} span-argument entry points, and {n_fg} finite-float domain gate modules."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
