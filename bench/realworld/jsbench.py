"""`rw.py js`: the scenes' JavaScript alone, node against a bronze-built
executable (README.md, "JS bench").

Each scene runs in mode=bench (lib/harness.js runBench): the browser page's
frame, scene step + library render, into the stub WebGL2 context of
lib/glstub.js, with no engine, no DOM and no GPU. node runs the scene file;
bronze builds it into .work/js/<scene>/<key>/ and runs the executable. A
bronze executable takes no arguments, so its configuration is compiled in
(cfg.js, imported before the scene by a generated main.js) and the build is
cached by the digest of everything it depends on: the bronze binary (mtime +
size), the flags, the configuration, and the scene, lib/ and vendor/ files.

Each run is pinned to one core (`taskset -c 4`) under `perf stat -e
instructions:u,cycles:u`, counting only the timed frames: the page prints a
marker before and after them (mark=1) whose write blocks on a pipe shrunk to
one page until this script has switched counting on (off) through perf's
control fifo. Each engine runs twice; the faster run is reported. The two
engines' checksums (scene state + the last frame's GL call counts) must
agree.
"""

import fcntl
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import rw

ROOT = rw.ROOT
JS_WORK = rw.WORK / "js"
DEFAULT_BRONZE = Path("/home/j/projects/bronze/build/src/cli/bronze")
CORE = "4"
COUNTERS = ["instructions:u", "cycles:u"]
# --frontend: what says a layout regression is one (README.md, "JS bench").
# The names are this machine's perf's (AMD Zen: `perf list`): L1 instruction-
# cache misses, op-cache misses, and the ops the legacy decoders delivered
# (each op-cache miss sends a fetch block through them).
FRONTEND_COUNTERS = ["ic_tag_hit_miss.instruction_cache_miss:u", "op_cache_hit_miss.op_cache_miss:u",
                     "de_src_op_disp.x86_decoder:u"]

# Engine-run sizes (rw.py time / the realworld bench numbers).
DEFAULT_N = {"three_meshes": 2565, "three_skinned": 410, "three_instanced": 42992, "three_lights": 64,
             "pixi_sprites": 10000, "pixi_graphics": 2000, "pixi_text": 400}
# (warm, frames): fewer for the scenes over ~30 ms a frame, so `all` stays short.
DEFAULT_FRAMES = {"pixi_graphics": (30, 100)}

F_SETPIPE_SZ = 1031


def file_digest(paths):
    h = hashlib.sha256()
    for p in sorted(paths):
        h.update(str(p.relative_to(ROOT)).encode())
        h.update(p.resolve().read_bytes())
    return h.hexdigest()


def sources_digest(scene):
    files = [ROOT / "scenes" / f"{scene}.js"] + sorted((ROOT / "lib").glob("*.js")) + sorted((ROOT / "vendor").iterdir())
    return file_digest(files)


def build_key(scene, cfg, bronze, flags, stage, gen=""):
    st = bronze.stat()
    h = hashlib.sha256()
    h.update(json.dumps([str(bronze), st.st_mtime_ns, st.st_size, flags, cfg, stage, sources_digest(scene),
                         hashlib.sha256(gen.encode()).hexdigest()]).encode())
    return h.hexdigest()[:16]


_GEN = {}


def generated_functions(scene, cfg):
    """gen.js for a pixi scene: the bodies pixi passes to `new Function` in a
    node run of the same configuration (lib/bench_env.js
    captureGeneratedFunctions), compiled in as ordinary functions, and a
    global `Function` that hands each back by its arguments. A bronze
    executable has no eval; this is the same code, compiled ahead of time.
    three.js generates no code: '' for its scenes."""
    if not scene.startswith("pixi"):
        return ""
    key = (scene, json.dumps(cfg, sort_keys=True))
    if key in _GEN:
        return _GEN[key]
    JS_WORK.mkdir(parents=True, exist_ok=True)
    cap = JS_WORK / scene / "capture.json"
    cap.parent.mkdir(parents=True, exist_ok=True)
    if cap.exists():
        cap.unlink()
    run_cfg = dict(cfg, mark=0, capture=str(cap))
    res = subprocess.run(["node", str(ROOT / "scenes" / f"{scene}.js")] + [f"{k}={v}" for k, v in run_cfg.items()],
                         capture_output=True, text=True)
    if res.returncode != 0 or not cap.exists():
        raise RuntimeError(f"node capture run of {scene} failed:\n" + (res.stdout + res.stderr)[-2000:])
    out = ["// The bodies pixi generated with `new Function` in a node run (rw.py js), as",
           "// ordinary functions: a bronze-built executable has no eval.",
           "const T = new Map();"]
    for k, args in json.loads(cap.read_text()):
        params, body = args[:-1], (args[-1] if args else "")
        out.append(f"T.set({json.dumps(k, ensure_ascii=False)}, function ({', '.join(params)}) {{\n{body}\n}});")
    out += ["const Real = globalThis.Function;",
            "function BenchFunction(...args) {",
            "    const k = JSON.stringify(args.map(String));",
            "    const f = T.get(k);",
            "    if (f !== undefined) return f;",
            "    throw new Error('mode=bench: new Function(' + k.slice(0, 300) + ') was not seen in the node capture run');",
            "}",
            "BenchFunction.prototype = Real.prototype;",
            "globalThis.Function = BenchFunction;", ""]
    _GEN[key] = "\n".join(out)
    return _GEN[key]


def bronze_build(scene, cfg, bronze, flags, stage="plain", extra=()):
    """The executable for (scene, cfg, flags, stage) in .work/js/<scene>/<key>/;
    returns (exe, seconds the build took, or None when it was cached)."""
    gen = generated_functions(scene, cfg)
    key = build_key(scene, cfg, bronze, [flags, list(extra)], stage, gen)
    d = JS_WORK / scene / key
    exe = d / "bench"
    if exe.exists() and (d / "bench.so").exists():
        return exe, None
    if d.exists():
        shutil.rmtree(d)
    d.mkdir(parents=True)
    (d / "cfg.js").write_text("globalThis.BENCH_CONFIG = " + json.dumps(cfg) + ";\n")
    rel = os.path.relpath(ROOT / "scenes" / f"{scene}.js", d)
    main = f"import './cfg.js';\nimport '{rel}';\n"
    if gen:
        (d / "gen.js").write_text(gen)
        main = "import './gen.js';\n" + main
    (d / "main.js").write_text(main)
    cmd = [str(bronze), "build", str(d / "main.js"), "-o", str(exe)] + flags.split() + list(extra)
    t0 = time.time()
    res = subprocess.run(cmd, cwd=str(d), capture_output=True, text=True)
    secs = round(time.time() - t0, 1)
    (d / "build.log").write_text(res.stdout + res.stderr)
    if res.returncode != 0 or not exe.exists():
        shutil.rmtree(d / "bench", ignore_errors=True)
        raise RuntimeError(f"bronze build {scene} failed ({d / 'build.log'}):\n" + (res.stdout + res.stderr)[-3000:])
    return exe, secs


def harvested_build(scene, cfg, bronze, flags):
    """--harvest: a plain build with --harvest-out, one run to write the
    harvest, then the build on it (--feedback-in), the one measured."""
    gen = generated_functions(scene, cfg)
    tfb = JS_WORK / scene / (build_key(scene, cfg, bronze, flags, "harvest", gen) + ".tfb")
    fed_flags = flags + " --feedback-in " + str(tfb)
    fed = JS_WORK / scene / build_key(scene, cfg, bronze, [fed_flags, []], "fed", gen) / "bench"
    if fed.exists() and tfb.exists():
        return fed, None
    exe1, secs1 = bronze_build(scene, cfg, bronze, flags, "harvest", ("--harvest-out", str(tfb)))
    if tfb.exists():
        tfb.unlink()
    res = subprocess.run([str(exe1)], capture_output=True, text=True)
    if res.returncode != 0 or not tfb.exists():
        raise RuntimeError(f"harvest run of {scene} wrote no {tfb}:\n" + (res.stdout + res.stderr)[-2000:])
    exe2, secs2 = bronze_build(scene, cfg, bronze, fed_flags, "fed")
    return exe2, round((secs1 or 0) + (secs2 or 0), 1)


def counted_run(cmd, perf_extra=None, events=None):
    """Run cmd pinned to one core under perf, counting `events` (COUNTERS by
    default) between the page's marks. Returns (stdout text, {event: n} or {})."""
    tmp = Path(tempfile.mkdtemp(prefix="rwjs_", dir=str(rw.WORK)))
    ctl, ack, stat = tmp / "ctl", tmp / "ack", tmp / "stat.txt"
    os.mkfifo(ctl)
    os.mkfifo(ack)
    ctl_fd = os.open(ctl, os.O_RDWR)
    ack_fd = os.open(ack, os.O_RDWR)
    perf = perf_extra or ["perf", "stat", "-x,", "-e", ",".join(events or COUNTERS), "-o", str(stat)]
    full = perf + ["-D", "-1", "--control", f"fifo:{ctl},{ack}", "--", "taskset", "-c", CORE] + cmd
    errf = open(tmp / "stderr.txt", "w")
    proc = subprocess.Popen(full, stdout=subprocess.PIPE, stderr=errf)
    out_fd = proc.stdout.fileno()
    try:
        fcntl.fcntl(out_fd, F_SETPIPE_SZ, 4096)
    except OSError:
        pass
    buf = bytearray()
    scan = 0
    marks = 0

    def control(word):
        os.write(ctl_fd, word.encode() + b"\n")
        os.read(ack_fd, 64)

    while True:
        chunk = os.read(out_fd, 4096)
        if not chunk:
            break
        buf += chunk
        while marks < 2:
            tag = b"RW mark on" if marks == 0 else b"RW mark off"
            i = buf.find(tag, max(0, scan - 16))
            if i < 0:
                scan = len(buf)
                break
            control("enable" if marks == 0 else "disable")
            marks += 1
            scan = i + len(tag)
    proc.wait()
    errf.close()
    os.close(ctl_fd)
    os.close(ack_fd)
    text = buf.decode(errors="replace")
    text = "\n".join(l if not l.startswith("RW mark") else l.rstrip() for l in text.splitlines())
    counts = {}
    if stat.exists():
        for line in stat.read_text().splitlines():
            parts = line.split(",")
            if len(parts) > 2 and parts[0].strip().isdigit():
                counts[parts[2]] = int(parts[0])
    err = (tmp / "stderr.txt").read_text(errors="replace")
    shutil.rmtree(tmp, ignore_errors=True)
    if proc.returncode != 0 or marks < 2:
        raise RuntimeError(f"run failed (rc={proc.returncode}, marks={marks}): {' '.join(cmd)}\n" + text[-1500:] + err[-1500:])
    return text, counts, tmp


def bench_line(text):
    for tag, obj in rw.rw_lines(text):
        if tag == "bench":
            return obj
    errs = [o for t, o in rw.rw_lines(text) if t == "error"]
    raise RuntimeError("no RW bench line" + (": " + json.dumps(errs[0])[:800] if errs else ""))


def measure(cmd, frames, reps=2, events=None):
    """Best of `reps` counted runs: (bench line, instructions/frame,
    cycles/frame, {event: count/frame})."""
    best = None
    for _ in range(reps):
        text, counts, _tmp = counted_run(cmd, events=events)
        b = bench_line(text)
        per = {k: v / frames for k, v in counts.items()}
        if best is None or b["ms_per_frame"] < best[0]["ms_per_frame"]:
            best = (b, per.get("instructions:u", 0), per.get("cycles:u", 0), per)
    return best


def scene_cfg(scene, args):
    warm, frames = DEFAULT_FRAMES.get(scene, (60, 300))
    if args.warm:
        warm = args.warm
    if args.frames:
        frames = args.frames
    return {"mode": "bench", "n": args.n or DEFAULT_N[scene], "warm": warm, "frames": frames, "mark": 1}


def fmt_n(x):
    if x >= 1e6:
        return f"{x / 1e6:.2f}M"
    if x >= 1e3:
        return f"{x / 1e3:.1f}k"
    return f"{x:.0f}"


def cmd_js(args):
    bronze = Path(args.bronze).resolve()
    scenes = rw.SCENES if args.scene == "all" else args.scene.split(",")
    if args.profile:
        for scene in scenes:
            profile(scene, args, bronze)
        return
    rows = []
    for scene in scenes:
        cfg = scene_cfg(scene, args)
        try:
            if args.harvest:
                exe, secs = harvested_build(scene, cfg, bronze, args.flags)
            else:
                exe, secs = bronze_build(scene, cfg, bronze, args.flags)
            node_cmd = ["node", str(ROOT / "scenes" / f"{scene}.js")] + [f"{k}={v}" for k, v in cfg.items()]
            nb, ni, nc, _ = measure(node_cmd, cfg["frames"])
            events = COUNTERS + FRONTEND_COUNTERS if args.frontend else None
            bb, bi, bc, bper = measure([str(exe)], cfg["frames"], events=events)
        except Exception as e:  # noqa: BLE001
            print(f"{scene}: ERROR {e}", flush=True)
            rows.append({"scene": scene, "error": str(e)[:2000]})
            continue
        ok = nb["checksum"] == bb["checksum"]
        row = {"scene": scene, "n": cfg["n"], "frames": cfg["frames"], "node_ms": nb["ms_per_frame"], "bronze_ms": bb["ms_per_frame"],
               "node_instr": ni, "bronze_instr": bi, "node_cycles": nc, "bronze_cycles": bc,
               "draws": bb["draw_calls_per_frame"], "gl_calls": bb["gl_calls_per_frame"],
               "checksum_node": nb["checksum"], "checksum_bronze": bb["checksum"], "checksum_ok": ok,
               "build_s": secs, "exe": str(exe), "harvest": bool(args.harvest)}
        if args.frontend:
            row.update({"bronze_l1i_miss": bper.get(FRONTEND_COUNTERS[0], 0),
                        "bronze_opcache_miss": bper.get(FRONTEND_COUNTERS[1], 0),
                        "bronze_decoder_ops": bper.get(FRONTEND_COUNTERS[2], 0)})
        rows.append(row)
        if not ok:
            print(f"!!!! {scene}: CHECKSUM MISMATCH node {nb['checksum']} bronze {bb['checksum']}"
                  f" (node last {nb['last']}, bronze last {bb['last']})", flush=True)
        print(f"   {scene}: node {nb['ms_per_frame']:.3f} ms, bronze {bb['ms_per_frame']:.3f} ms"
              + (f", built in {secs} s" if secs is not None else ", cached build"), flush=True)
    print()
    title = "bronze" + (" (harvest)" if args.harvest else "")
    fe_head = f" {'cyc/f':>8} {'L1I miss/f':>10} {'op$ miss/f':>10} {'dec ops/f':>9}" if args.frontend else ""
    print(f"{'scene':16} {'n':>6} {'node ms':>8} {title + ' ms':>{max(10, len(title) + 3)}} {'ratio':>6}"
          f" {'node ins/f':>11} {'bronze ins/f':>12} {'ratio':>6}{fe_head} {'draws':>6} {'gl calls':>8}  check  build")
    for r in rows:
        if "error" in r:
            print(f"{r['scene']:16} ERROR: {r['error'].splitlines()[0][:100]}")
            continue
        ratio = r["bronze_ms"] / r["node_ms"] if r["node_ms"] else 0
        iratio = r["bronze_instr"] / r["node_instr"] if r["node_instr"] else 0
        build = "cached" if r["build_s"] is None else f"{r['build_s']} s"
        fe = (f" {fmt_n(r['bronze_cycles']):>8} {fmt_n(r['bronze_l1i_miss']):>10} {fmt_n(r['bronze_opcache_miss']):>10}"
              f" {fmt_n(r['bronze_decoder_ops']):>9}") if args.frontend else ""
        print(f"{r['scene']:16} {r['n']:>6} {r['node_ms']:>8.3f} {r['bronze_ms']:>{max(10, len(title) + 3)}.3f} {ratio:>6.2f}"
              f" {fmt_n(r['node_instr']):>11} {fmt_n(r['bronze_instr']):>12} {iratio:>6.2f}{fe} {r['draws']:>6.0f} {r['gl_calls']:>8.0f}"
              f"  {'ok' if r['checksum_ok'] else 'MISMATCH'}  {build}")
    if any(not r.get("checksum_ok", True) for r in rows):
        print("\n!!!! CHECKSUM MISMATCH: node and bronze did not do the same work (see above)")
    if args.json:
        Path(args.json).write_text(json.dumps(rows, indent=1))


def profile(scene, args, bronze):
    """perf record of the bronze executable's timed frames; top 25 symbols."""
    cfg = scene_cfg(scene, args)
    if args.harvest:
        exe, _ = harvested_build(scene, cfg, bronze, args.flags)
    else:
        exe, _ = bronze_build(scene, cfg, bronze, args.flags)
    data = JS_WORK / scene / "perf.data"
    perf = ["perf", "record", "-q", "-e", "cycles:u", "-F", "4999", "-o", str(data)]
    text, _, _ = counted_run([str(exe)], perf_extra=perf)
    b = bench_line(text)
    print(f"{scene} n={cfg['n']}: bronze {b['ms_per_frame']:.3f} ms/frame ({exe})")
    res = subprocess.run(["perf", "report", "-i", str(data), "--stdio", "--no-children", "--sort", "dso,symbol",
                          "-q", "--percent-limit", "0.2"], capture_output=True, text=True)
    lines = [l for l in res.stdout.splitlines() if l.strip()]
    for l in lines[:25]:
        print(l[:160])


def add_commands(sub):
    p = sub.add_parser("js", help="scene JS alone: node vs a bronze executable (README.md, JS bench)")
    p.add_argument("scene", help="a scene, a comma list, or all")
    p.add_argument("--n", type=int, default=0)
    p.add_argument("--warm", type=int, default=0)
    p.add_argument("--frames", type=int, default=0)
    p.add_argument("--bronze", default=str(DEFAULT_BRONZE))
    p.add_argument("--flags", default="--lockdown",
                   help="`bronze build` flags (default --lockdown: locked builtins, what bro gives its apps)")
    p.add_argument("--harvest", action="store_true", help="build, run to harvest the inline caches, rebuild on them")
    p.add_argument("--profile", action="store_true", help="perf record the bronze executable's timed frames; top 25 symbols")
    p.add_argument("--frontend", action="store_true",
                   help="also count the bronze run's L1I misses, op-cache misses and decoder ops per frame")
    p.add_argument("--json", default="")
    return {"js": cmd_js}


if __name__ == "__main__":
    sys.exit("run through rw.py: python3 bench/realworld/rw.py js <scene>|all")
