"""Frame timing for the real-world scenes: rw.py's `time`, `sweep` and `perf`
commands (mode=time in lib/harness.js, lib/timing.js).

  rw.py time  --browser chromium|bro|bro-aot --scene S --n N [--json F]
      One windowed run: warm frames, then timed ones; prints the summary.
  rw.py sweep [--scenes a,b] [--out DIR] [--rest 20] [--light-rest 5]
      Per scene, N steps geometrically (x1.5) from the scene's minimum until
      Chromium's median frame interval passes 16.7 ms and two steps beyond;
      bro (JIT and AOT) at the same N. Each point runs twice (a third time
      when the two medians differ by more than 10%), one browser at a time,
      with a fixed rest after each run (`--rest` after a GPU-heavy one).
      Writes DIR/timing.json (DIR/sweep.partial.json while it runs).
  rw.py perf --scene S --n N [--aot] [--out DIR]
      One bro run of a longer timed window under `perf record`; the samples
      inside the timed window are grouped (JIT code, bronze runtime, bro
      WebGL, Vulkan driver, Skia, compositor, ...) next to the flight
      recorder's phase split of the same frames.
"""

import json
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path

import rw

KNEE_MS = 1000.0 / 60.0


# ---------------------------------------------------------------- statistics

def pct(xs, p):
    """Nearest-rank percentile of a list (None values dropped)."""
    v = sorted(x for x in xs if x is not None)
    if not v:
        return None
    k = max(0, min(len(v) - 1, int(round(p / 100.0 * len(v) + 0.5)) - 1))
    return v[k]


def summary(xs):
    v = [x for x in xs if x is not None] if xs else []
    if not v:
        return None
    return {"median": round(statistics.median(v), 4), "p95": round(pct(v, 95), 4), "p99": round(pct(v, 99), 4),
            "mean": round(statistics.fmean(v), 4), "min": round(min(v), 4), "max": round(max(v), 4), "count": len(v)}


# Flight-recorder phases (docs/agent-control.md "The flight recorder"),
# each summarised over the timed frames. `total` is the record's whole frame.
TRACE_PHASES = ["total", "events", "control", "misc", "input", "tick", "js", "layoutWait", "record",
                "composite", "present", "gpuWait", "flipWait", "pacingWait", "gc", "other"]


def trace_summary(path):
    """The frame records of `bro-ctl trace --json --frames` summarised per
    phase (ms), plus how many presented and the present rate over the window."""
    try:
        doc = json.loads(Path(path).read_text())
    except (OSError, json.JSONDecodeError):
        return None
    frames = doc.get("frames") if isinstance(doc, dict) else None
    if not frames:
        return None
    out = {"frames": len(frames), "refreshMs": doc.get("refreshMs")}
    for ph in TRACE_PHASES:
        vals = [f.get(ph) for f in frames if isinstance(f.get(ph), (int, float))]
        if vals:
            out[ph] = summary(vals)
    pres = sorted(f["presentAt"] for f in frames if f.get("presented") and f.get("presentAt"))
    out["presented"] = len(pres)
    if len(pres) > 1:
        out["presentsPerSec"] = round((len(pres) - 1) * 1000.0 / (pres[-1] - pres[0]), 2)
    return out


# ---------------------------------------------------------------- one run

MODES = {
    "chromium": ("chromium", False),
    "bro": ("bro", False),
    "bro-aot": ("bro", True),
}


def run_time(mode, scene, n, warm=120, meas=300, gpu=1, perf_data=None, tag=""):
    """One timed run; returns a dict with the raw per-frame arrays and their summaries."""
    browser, aot = MODES[mode]
    page = f"{scene}.html"
    frames_ms_guess = 600000
    cfg = {"mode": "time", "n": n, "warm": warm, "meas": meas, "gpu": gpu, "watchdog": frames_ms_guess}
    name = f"time_{mode}_{scene}"
    t0 = time.time()
    if browser == "bro":
        status, log, out = rw.run_bro(page, cfg, name, aot=aot, perf_data=perf_data)
    else:
        status, log, out = rw.windowed("chromium", page, cfg, name, rw.Server())
    wall = time.time() - t0
    lines = rw.rw_lines("\n".join(log))
    timing = next((o for t, o in lines if t == "timing"), None)
    errors = [o for t, o in lines if t == "error"]
    res = {"mode": mode, "scene": scene, "n": n, "status": status, "wall_s": round(wall, 2), "tag": tag}
    if errors:
        res["errors"] = errors[:4]
    # A program that failed to compile or link draws nothing: the point is
    # not the same load (three_lights past Chromium's 1024 fragment uniform
    # vectors, for one).
    shader = [l.strip()[:300] for l in log if "Shader Error" in l or "Program Info Log" in l]
    if shader:
        res["shader_error"] = shader[:3]
    if not isinstance(timing, dict):
        res["ok"] = False
        res["log_tail"] = [l for l in log if "rror" in l][-6:]
        return res
    res["ok"] = status == "ok" and not res.get("shader_error")
    res["raw"] = {k: timing.get(k) for k in ("interval", "raf", "cpu", "gpu", "done")}
    res["raf_ms"] = summary(timing.get("raf"))
    res["interval_ms"] = summary(timing["interval"])
    res["cpu_ms"] = summary(timing["cpu"])
    res["gpu_ms"] = summary(timing["gpu"]) if timing.get("gpu") else None
    res["delivered_ms"] = timing.get("delivered")
    res["done_ms"] = summary(timing.get("done"))
    iv = res["interval_ms"]["median"] if res["interval_ms"] else None
    d = res["delivered_ms"]
    # The frame time a point is judged by: a frame is done when both its
    # callback has run (the rAF interval) and the GPU has finished it.
    res["frame_ms"] = round(max(iv, d), 4) if (iv is not None and d is not None) else iv
    res["meta"] = {k: timing.get(k) for k in ("rafSpan", "drainFrames", "clock", "gpuExt", "disjoint", "unresolved", "focus", "traceWindow", "inner", "ua")}
    if browser == "bro":
        tr = out / "trace.json"
        if tr.exists():
            res["trace"] = trace_summary(tr)
            res["trace_path"] = str(tr)
    return res


def cmd_time(args):
    res = run_time(args.browser, args.scene, args.n, warm=args.warm, meas=args.meas, gpu=args.gpu)
    brief = {k: v for k, v in res.items() if k != "raw"}
    print(json.dumps(brief, indent=1))
    if args.json:
        Path(args.json).write_text(json.dumps(res, indent=1))


# ---------------------------------------------------------------- perf

def perf_group(dso, sym):
    """Which part of the stack a sample is in, from its DSO and symbol."""
    d = os.path.basename(dso or "")
    s = sym or ""
    if d.startswith(("[JIT]", "perf-", "jitted-")) or d == "app.so":
        return "js-compiled"
    if "bronze_runtime" in d or "libbronze" in d:
        # brass:: is the JIT compiler (tier-up compiles on its threads)
        return "jit-compiler" if s.startswith("brass::") and not s.startswith(("brass::math::", "brass::rt")) else "bronze-runtime"
    if "vulkan_radeon" in d or d.startswith(("libvulkan", "libdrm", "libVkLayer")) or "radv" in s.lower():
        return "vulkan-driver"
    if d.startswith(("libSDL3", "libwayland", "libxkb", "libEGL", "libdecor")):
        return "window-system"
    if d.startswith(("libc.so", "libm.so", "libstdc++", "libgcc", "ld-linux")):
        return "libc"
    if d == "libbro.so" or d.startswith("bro"):
        if "bro::webgl" in s:
            return "bro-webgl"
        if "bro::bronze_host" in s:
            return "bro-bindings"
        if "brass::" in s and "brass::math::" not in s:
            return "jit-compiler"
        if "bronze::" in s or s.startswith(("bronze_", "brass_", "bz_")):
            return "bronze-runtime"
        if s.startswith(("Sk", "Gr", "sk_", "skgpu", "skia")) or "skgpu::" in s or "Sk" in s.split("::")[0]:
            return "skia"
        if "bro::render" in s or "bro::compositor" in s or "Presenter" in s:
            return "compositor-present"
        if "glslang" in s or "spv" in s.lower():
            return "shader-compile"
        if "bro::engine" in s or "bro::platform" in s:
            return "bro-engine"
        if "htmlayout" in s or "bro::layout" in s or "bro::dom" in s:
            return "bro-layout-dom"
        return "bro-other"
    if d in ("[unknown]", ""):
        return "unknown"
    return "other:" + d


def perf_report(data, t0_ms, t1_ms):
    """Samples inside [t0, t1] (CLOCK_MONOTONIC ms) by thread, DSO and
    symbol, and their sum by group, per thread and overall."""
    cmd = ["perf", "report", "-i", str(data), "--stdio", "--no-children", "-t", "\t",
           "-F", "sample,pid,dso,sym", "--percent-limit", "0",
           f"--time={t0_ms / 1000.0:.6f},{t1_ms / 1000.0:.6f}"]
    res = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    rows = []
    for line in res.stdout.splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) < 4:
            continue
        try:
            n = int(parts[0].strip())
        except ValueError:
            continue
        # `pid` is per thread: "<tid>:<comm>"; the main thread's tid is the process id
        comm, dso, sym = parts[1].strip(), parts[2].strip(), parts[3].strip()
        if sym.startswith("[.] ") or sym.startswith("[k] "):
            sym = sym[4:]
        rows.append({"samples": n, "thread": comm, "dso": dso, "sym": sym, "group": perf_group(dso, sym)})
    tids = [int(r["thread"].split(":", 1)[0]) for r in rows if r["thread"].split(":", 1)[0].isdigit()]
    main_tid = min(tids) if tids else None
    for r in rows:
        tid = r["thread"].split(":", 1)[0]
        r["main"] = tid.isdigit() and int(tid) == main_tid
    total = sum(r["samples"] for r in rows) or 1
    groups, threads, main = {}, {}, {}
    for r in rows:
        groups[r["group"]] = groups.get(r["group"], 0) + r["samples"]
        if r["main"]:
            main[r["group"]] = main.get(r["group"], 0) + r["samples"]
        t = threads.setdefault(r["thread"], {})
        t[r["group"]] = t.get(r["group"], 0) + r["samples"]
    pctd = lambda d: {k: round(100.0 * v / total, 2) for k, v in sorted(d.items(), key=lambda kv: -kv[1])}
    rows.sort(key=lambda r: -r["samples"])
    return {
        "samples": total,
        "groups_pct": pctd(groups),
        "main_thread_pct": pctd(main),
        "main_thread_share_pct": round(100.0 * sum(main.values()) / total, 2),
        # the main thread's groups as shares of the main thread alone
        "main_thread_norm_pct": {k: round(100.0 * v / (sum(main.values()) or 1), 2) for k, v in sorted(main.items(), key=lambda kv: -kv[1])},
        "threads_pct": {k: pctd(v) for k, v in sorted(threads.items(), key=lambda kv: -sum(kv[1].values()))},
        "top": [dict(r, pct=round(100.0 * r["samples"] / total, 2)) for r in rows[:60]],
        "stderr": res.stderr[-400:] if res.returncode else "",
    }


def run_perf(scene, n, aot, out_dir, meas=600):
    """A bro run of `meas` timed frames under perf, reported over the timed window."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    mode = "bro-aot" if aot else "bro"
    data = out_dir / f"perf_{mode}_{scene}_{n}.data"
    started = time.time()
    res = run_time(mode, scene, n, warm=120, meas=meas, perf_data=data, tag="perf")
    win = (res.get("meta") or {}).get("traceWindow")
    if res.get("ok") and win and None not in win:
        res["perf"] = perf_report(data, win[0], win[1])
    else:
        res["perf"] = None
    res.pop("raw", None)
    # The profile and brass's map of the run's JIT code (/tmp/perf-<pid>.map,
    # where perf looks for it) are large: gone once reported.
    try:
        data.unlink()
    except OSError:
        pass
    for m in Path("/tmp").glob("perf-*.map"):
        try:
            if m.stat().st_mtime >= started - 1:
                m.unlink()
        except OSError:
            pass
    return res


def cmd_perf(args):
    res = run_perf(args.scene, args.n, args.aot, args.out, meas=args.meas)
    p = res.get("perf") or {}
    print(json.dumps({k: res.get(k) for k in ("mode", "scene", "n", "status", "interval_ms", "cpu_ms", "gpu_ms")}, indent=1))
    print("groups %:", json.dumps(p.get("groups_pct"), indent=1))
    print("main thread %% of all samples: %s; its groups:" % p.get("main_thread_share_pct"), json.dumps(p.get("main_thread_pct"), indent=1))
    print("threads %:", json.dumps({k: sum(v.values()) for k, v in (p.get("threads_pct") or {}).items()}, indent=1))
    for r in (p.get("top") or [])[:30]:
        print(f"  {r['pct']:6.2f}%  {r['group']:18} {r['thread'][:14]:14} {os.path.basename(r['dso'])[:24]:24} {r['sym'][:90]}")
    if args.json:
        Path(args.json).write_text(json.dumps(res, indent=1))


# ---------------------------------------------------------------- sweep

STEP = 1.5
MAX_STEPS = 26
N_CAP_FACTOR = 20          # Chromium N never passes this times rw.SIZES max
BRO_MAX_FRAME_MS = 1500.0   # a bro point predicted slower than this is skipped
BRO_RUN_BUDGET_S = 14.0     # timed frames of a slow bro point fill about this


def frames_for(pred_ms):
    """(warm, meas) for a point predicted at pred_ms a frame: 120 + 300 while
    that fits ~4 s + ~10 s, fewer (never under 30 + 60) for slower points."""
    if not pred_ms or pred_ms <= 0:
        return 120, 300
    warm = 120 if pred_ms * 120 <= 4000 else max(30, int(4000 / pred_ms))
    meas = 300 if pred_ms * 300 <= BRO_RUN_BUDGET_S * 1000 else max(60, int(BRO_RUN_BUDGET_S * 1000 / pred_ms))
    return warm, meas


def rest_after(res, rest, light_rest):
    """The fixed rest after a run. GPU-heavy (the timer query says the GPU
    was busy over 30% of the frame, or frames ran under 2 ms, where the
    presents alone keep it busy) rests for as long as the run took, at least
    light_rest and at most `rest`, so a load is never on for more than half
    the time nor for long; anything else rests light_rest."""
    iv = res.get("frame_ms")
    g = (res.get("gpu_ms") or {}).get("median")
    heavy = (iv and g and g / iv > 0.3) or (iv is not None and iv < 2.0)
    if heavy:
        return min(rest, max(light_rest, res.get("wall_s", rest)))
    return light_rest


def point_value(runs):
    """A point from its runs: each metric is the median over the runs of that
    run's statistic; `rep` is the run whose median interval is the median."""
    ok = [r for r in runs if r.get("ok") and r.get("interval_ms")]
    if not ok:
        return None
    med = lambda xs: round(statistics.median(xs), 4) if xs else None
    out = {"frame_ms": med([r["frame_ms"] for r in ok if r.get("frame_ms") is not None]),
           "delivered_ms": med([r["delivered_ms"] for r in ok if r.get("delivered_ms") is not None])}
    for metric in ("interval_ms", "cpu_ms", "gpu_ms", "raf_ms"):
        rs = [r[metric] for r in ok if r.get(metric)]
        if rs:
            out[metric] = {k: med([x[k] for x in rs]) for k in ("median", "p95", "p99", "mean")}
    srt = sorted(ok, key=lambda r: r.get("frame_ms") or r["interval_ms"]["median"])
    rep = srt[len(srt) // 2]
    out["rep_run"] = rep["run_id"]
    if rep.get("trace"):
        out["trace"] = rep["trace"]
    return out


def spread(runs):
    m = [r.get("frame_ms") or r["interval_ms"]["median"] for r in runs if r.get("ok") and r.get("interval_ms")]
    if len(m) < 2:
        return None
    return (max(m) - min(m)) / min(m) if min(m) > 0 else None


class Sweep:
    def __init__(self, out_dir, rest, light_rest, reps):
        self.out = Path(out_dir)
        self.out.mkdir(parents=True, exist_ok=True)
        self.rest, self.light_rest, self.reps = rest, light_rest, reps
        self.raw = {}
        self.doc = {
            "schema": "bro-realworld-timing/1",
            "description": ("Windowed frame timing of real-world three.js / pixi.js scenes in Chromium and bro "
                            "(bench/realworld, rw.py sweep). Per point: the frame interval (wall clock between "
                            "consecutive rAF callback starts), the CPU time of the rAF callback (scene update + "
                            "render), the GPU time of its WebGL work (EXT_disjoint_timer_query_webgl2), in ms, "
                            "as median / p95 / p99 / mean over the timed frames of a run, then the median of "
                            "those over the point's runs. bro points carry the flight recorder's phase split "
                            "(docs/agent-control.md) of the representative run."),
            "units": "ms",
            "machine": machine_info(),
            "method": {
                "warm_frames": 120, "timed_frames": 300,
                "slow_points": "bro points predicted slower than ~47 ms a frame time fewer frames (at least 30 warm + 60 timed), filling ~4 s + ~14 s",
                "step": STEP, "knee_ms": round(KNEE_MS, 4),
                "reps": "two runs per point, a third when their medians differ by more than 10%",
                "rest": f"fixed rest after each run: GPU-heavy runs rest as long as they ran, {light_rest}-{rest} s; others {light_rest} s",
                "uncapped": {"chromium": "--disable-gpu-vsync --disable-frame-rate-limit",
                             "bro": "bro.json vsync false, maxFps 0 (MAILBOX/IMMEDIATE swapchain)"},
                "window": "1280x720 canvas, devicePixelRatio 1; Chromium --app window --window-size=1280,720, bro.json width/height 1280x720",
                "clocks": {"chromium": "performance.now() (100 us resolution, not cross-origin isolated)",
                           "bro": "__host.wallMs() (steady_clock); bro's performance.now()/rAF timestamp are its frame clock, constant within a frame and snapped to the predicted vblank"},
                "modes": {"chromium": "Chromium (V8, WebGL2 via ANGLE on OpenGL ES / radeonsi)",
                          "bro": "bro folder app: scripts compiled in-process by bronze, default tiered JIT",
                          "bro-aot": "bro with app.so: the module graph compiled ahead of time by `bronze build --emit-shared`"},
            },
            "scenes": {},
        }

    def save(self, final=False):
        name = "timing.json" if final else "sweep.partial.json"
        (self.out / name).write_text(json.dumps(self.doc, indent=1))
        (self.out / "timing_raw.json").write_text(json.dumps(self.raw))

    def point(self, mode, scene, n, warm=120, meas=300):
        runs = []
        for i in range(self.reps + 1):
            if i == self.reps:
                sp = spread(runs)
                if sp is None or sp <= 0.10:
                    break
            res = run_time(mode, scene, n, warm=warm, meas=meas)
            res["run_id"] = f"{scene}/{mode}/{n}/{len(runs)}"
            self.raw[res["run_id"]] = res.pop("raw", None)
            res["warm"], res["meas"] = warm, meas
            runs.append(res)
            iv = (res.get("interval_ms") or {}).get("median")
            print(f"  {mode:9} {scene:16} N={n:<8} run {len(runs)}: frame {res.get('frame_ms')} ms  raf-interval {iv}  delivered {res.get('delivered_ms')}  cpu {(res.get('cpu_ms') or {}).get('median')}  "
                  f"gpu {(res.get('gpu_ms') or {}).get('median')}  status {res.get('status')}  ({res.get('wall_s')} s)", flush=True)
            if not res.get("ok"):
                break
            time.sleep(rest_after(res, self.rest, self.light_rest))
        return {"n": n, "runs": runs, "value": point_value(runs), "spread": spread(runs)}

    def scene(self, scene, modes, reuse=False):
        lo = rw.SIZES[scene][0]
        sc = self.doc["scenes"].setdefault(scene, {"points": {}})
        for m in modes:
            sc["points"][m] = []
        if reuse and sc.get("n_values"):
            # Keep Chromium's N values (and every other mode's points): only
            # `modes` run again.
            return self.bro_modes(scene, sc, modes)
        # Chromium sets the N values: from the minimum until its median frame
        # interval passes the knee, and two steps beyond.
        ns, past, n = [], -1, lo
        while len(ns) < MAX_STEPS:
            p = self.point("chromium", scene, n)
            sc["points"]["chromium"].append(p)
            self.save()
            v = p["value"]
            if v is None:
                r0 = p["runs"][0] if p["runs"] else {}
                sc["stopped"] = f"Chromium failed at N={n}: " + str(r0.get("shader_error") or r0.get("errors") or r0.get("status"))[:300]
                break
            ns.append(n)
            if past < 0 and v["frame_ms"] > KNEE_MS:
                past = 0
            elif past >= 0:
                past += 1
            if past >= 2:
                break
            n = max(n + 1, int(round(n * STEP)))
            if n > N_CAP_FACTOR * rw.SIZES[scene][1]:
                sc["stopped"] = f"N cap ({N_CAP_FACTOR} x the planned max {rw.SIZES[scene][1]}) reached"
                break
        sc["n_values"] = ns
        return self.bro_modes(scene, sc, modes)

    def bro_modes(self, scene, sc, modes):
        ns = sc["n_values"]
        for mode in modes:
            if mode == "chromium":
                continue
            last = None
            for n in ns:
                if last:
                    pred = last[1] * n / last[0]
                else:
                    c = next(pp for pp in sc["points"]["chromium"] if pp["n"] == n)
                    pred = ((c["value"] or {}).get("frame_ms") or 5.0) * 6
                if pred > BRO_MAX_FRAME_MS:
                    sc["points"][mode].append({"n": n, "skipped": f"predicted {pred:.0f} ms a frame"})
                    continue
                warm, meas = frames_for(pred)
                p = self.point(mode, scene, n, warm, meas)
                sc["points"][mode].append(p)
                self.save()
                if p["value"]:
                    last = (n, p["value"]["frame_ms"])
        allm = list(sc["points"].keys())
        sc["knees"] = {m: knee(sc["points"][m]) for m in allm}
        if scene in rw.AOT_BUILD_S:
            sc["aot_build_s"] = rw.AOT_BUILD_S[scene]
        ck = sc["knees"].get("chromium") or {}
        sc["at_chromium_knee"] = at_knee(sc, allm, ck)
        self.save()


def series(points):
    return [(p["n"], p["value"]["frame_ms"]) for p in points if p.get("value") and p["value"].get("frame_ms")]


def knee(points, limit=KNEE_MS):
    """The first swept N whose median frame interval passes `limit`, the last
    N under it, and the N where the curve crosses it (log-log interpolation
    between those two)."""
    s = series(points)
    over = next((i for i, (n, v) in enumerate(s) if v > limit), None)
    if over is None:
        return {"first_over": None, "last_under": s[-1][0] if s else None, "crossing_n": None,
                "note": "never passed %.2f ms in the swept range" % limit}
    if over == 0:
        return {"first_over": s[0][0], "last_under": None, "crossing_n": None,
                "note": "already over %.2f ms at the smallest N" % limit}
    (n0, v0), (n1, v1) = s[over - 1], s[over]
    import math
    if v0 > 0 and v1 > v0:
        t = (math.log(limit) - math.log(v0)) / (math.log(v1) - math.log(v0))
        cross = math.exp(math.log(n0) + t * (math.log(n1) - math.log(n0)))
    else:
        cross = n1
    return {"first_over": n1, "last_under": n0, "crossing_n": round(cross)}


def at_knee(sc, modes, ck):
    """Each mode's numbers at Chromium's knee N (the first N past 16.7 ms),
    and bro / Chromium ratios of the frame interval and the callback's CPU time."""
    n = ck.get("first_over")
    if n is None:
        return None
    vals = {}
    for m in modes:
        p = next((pp for pp in sc["points"][m] if pp["n"] == n and pp.get("value")), None)
        if p:
            vals[m] = {"frame_ms": p["value"]["frame_ms"], "delivered_ms": p["value"].get("delivered_ms"),
                       "interval_ms": p["value"]["interval_ms"], "cpu_ms": p["value"].get("cpu_ms"),
                       "gpu_ms": p["value"].get("gpu_ms"), "trace": p["value"].get("trace")}
    out = {"n": n, "values": vals, "ratio_frame": {}, "ratio_cpu": {}}
    c = vals.get("chromium")
    for m in modes:
        if m == "chromium" or m not in vals or not c:
            continue
        out["ratio_frame"][m] = round(vals[m]["frame_ms"] / c["frame_ms"], 3)
        if vals[m].get("cpu_ms") and c.get("cpu_ms") and c["cpu_ms"]["median"]:
            out["ratio_cpu"][m] = round(vals[m]["cpu_ms"]["median"] / c["cpu_ms"]["median"], 3)
    return out


def machine_info():
    info = {"host": os.uname().nodename, "kernel": os.uname().release}
    try:
        res = subprocess.run(["chromium", "--version"], capture_output=True, text=True)
        info["chromium"] = res.stdout.strip()
    except OSError:
        pass
    try:
        res = subprocess.run(["git", "-C", str(rw.BRO_ROOT), "rev-parse", "--short", "HEAD"], capture_output=True, text=True)
        info["bro_commit"] = res.stdout.strip()
    except OSError:
        pass
    try:
        res = subprocess.run([str(rw.BUILD / "bro-ctl"), "-s", "display", "info"], capture_output=True, text=True)
        d = json.loads(res.stdout)
        info["display"] = {"refreshMs": d.get("refreshMs"), "size": d.get("viewport"), "compositor": d.get("app"), "mode": d.get("mode")}
    except (OSError, ValueError):
        pass
    info["cpu"] = next((l.split(":", 1)[1].strip() for l in Path("/proc/cpuinfo").read_text().splitlines() if l.startswith("model name")), "")
    info["gpu"] = "AMD Radeon 8060S (RADV in bro, radeonsi via ANGLE GL ES in Chromium)"
    return info


def cmd_sweep(args):
    scenes = args.scenes.split(",") if args.scenes else rw.SCENES
    modes = args.modes.split(",")
    sw = Sweep(args.out, args.rest, args.light_rest, args.reps)
    prev = Path(args.out) / "timing.json"
    if args.merge and prev.exists():
        sw.doc["scenes"] = json.loads(prev.read_text()).get("scenes", {})
        rawp = Path(args.out) / "timing_raw.json"
        if rawp.exists():
            sw.raw = json.loads(rawp.read_text())
    t0 = time.time()
    for scene in scenes:
        print(f"== {scene}", flush=True)
        if not args.reuse:
            sw.doc["scenes"].pop(scene, None)
        sw.scene(scene, modes, reuse=args.reuse)
        sc = sw.doc["scenes"][scene]
        print(f"   knees: {json.dumps(sc['knees'])}", flush=True)
        print(f"   at chromium knee: {json.dumps((sc.get('at_chromium_knee') or {}).get('ratio_frame'))}", flush=True)
        if args.perf and sc.get("at_chromium_knee"):
            kn = sc["at_chromium_knee"]["n"]
            sc.setdefault("perf_at_knee", {})
            for mode in modes:
                if mode == "chromium":
                    continue
                v = (sc["at_chromium_knee"]["values"].get(mode) or {}).get("frame_ms")
                meas = max(60, min(600, int(4000 / v))) if v else 120
                r = run_perf(scene, kn, mode == "bro-aot", Path(args.out) / "perf", meas=meas)
                sc["perf_at_knee"][mode] = {k: r.get(k) for k in ("n", "status", "interval_ms", "cpu_ms", "gpu_ms", "trace", "perf")}
                g = (r.get("perf") or {}).get("main_thread_norm_pct")
                print(f"   perf {mode} N={kn}: main thread {json.dumps(g)}", flush=True)
                sw.save()
                time.sleep(args.rest)
    sw.doc["elapsed_s"] = round(time.time() - t0)
    sw.save(final=True)
    try:
        (Path(args.out) / "sweep.partial.json").unlink()
    except OSError:
        pass
    print("wrote", Path(args.out) / "timing.json")


# ---------------------------------------------------------------- report

def fmt(v, nd=2):
    return "-" if v is None else f"{v:.{nd}f}"


def report_md(doc):
    """timing.json as markdown: per scene, N against frame time per mode,
    the knees, the ratios at Chromium's knee, the flight-recorder split and
    the perf groups there."""
    L = ["# Real-world scenes: frame timing, bro vs Chromium", ""]
    m = doc.get("machine", {})
    L.append(f"Machine: {m.get('host')} ({m.get('cpu')}; {m.get('gpu')}), display {m.get('display')}, "
             f"{m.get('chromium')}, bro {m.get('bro_commit')}.")
    L.append("")
    L.append("frame = max(median rAF interval, delivered), where delivered is the timed frames' GPU-completion span "
             "per frame; cpu = the rAF callback's median wall time; ms.")
    L.append("")
    for scene, sc in doc.get("scenes", {}).items():
        modes = list(sc["points"].keys())
        L.append(f"## {scene}")
        L.append("")
        hdr = "| N | " + " | ".join(f"{mo} frame | {mo} cpu | {mo} gpu" for mo in modes) + " |"
        L.append(hdr)
        L.append("|" + "---|" * (1 + 3 * len(modes)))
        by = {mo: {p["n"]: p for p in sc["points"][mo]} for mo in modes}
        for n in sc.get("n_values", []):
            cells = []
            for mo in modes:
                p = by[mo].get(n) or {}
                v = p.get("value") or {}
                if p.get("skipped"):
                    cells += ["skip", "", ""]
                    continue
                cells += [fmt(v.get("frame_ms")), fmt((v.get("cpu_ms") or {}).get("median")), fmt((v.get("gpu_ms") or {}).get("median"))]
            L.append(f"| {n} | " + " | ".join(cells) + " |")
        L.append("")
        kn = sc.get("knees", {})
        L.append("60 fps knee (first N over 16.7 ms / interpolated crossing): " + "; ".join(
            f"{mo} {k.get('first_over')} / {k.get('crossing_n')}" + (f" ({k['note']})" if k.get("note") else "") for mo, k in kn.items()))
        if sc.get("stopped"):
            L.append("")
            L.append("Sweep stopped: " + sc["stopped"])
        ak = sc.get("at_chromium_knee")
        if ak:
            L.append("")
            L.append(f"At Chromium's knee N={ak['n']}: frame ratio bro/Chromium " +
                     ", ".join(f"{mo} {r}x" for mo, r in ak["ratio_frame"].items()) + "; callback CPU ratio " +
                     ", ".join(f"{mo} {r}x" for mo, r in ak["ratio_cpu"].items()) + ".")
            for mo, v in ak["values"].items():
                tr = v.get("trace")
                if not tr:
                    continue
                g = lambda k: (tr.get(k) or {}).get("median")
                L.append(f"- {mo} flight recorder (median ms): total {fmt(g('total'))}, input {fmt(g('input'))}, "
                         f"tick {fmt(g('tick'))} (js {fmt(g('js'))}), composite {fmt(g('composite'))}, present {fmt(g('present'))}, "
                         f"gc {fmt(g('gc'))}; presents/s {tr.get('presentsPerSec')}")
        pk = sc.get("perf_at_knee")
        if pk:
            L.append("")
            for mo, r in pk.items():
                p = (r or {}).get("perf") or {}
                if not p:
                    L.append(f"- perf {mo}: no profile ({(r or {}).get('status')})")
                    continue
                g = p.get("main_thread_norm_pct", {})
                L.append(f"- perf {mo}, main thread ({p.get('main_thread_share_pct')}% of all samples): " +
                         ", ".join(f"{k} {v}%" for k, v in list(g.items())[:9]))
                o = p.get("groups_pct", {})
                L.append(f"  all threads: " + ", ".join(f"{k} {v}%" for k, v in list(o.items())[:8]))
                top = [t for t in p.get("top", []) if t.get("main")][:8]
                L.append("  top main-thread symbols: " + "; ".join(f"{t['sym'][:60]} {t['pct']}%" for t in top))
        L.append("")
    return "\n".join(L) + "\n"


def cmd_report(args):
    doc = json.loads(Path(args.json).read_text())
    md = report_md(doc)
    if args.out:
        Path(args.out).write_text(md)
    print(md)


def add_commands(sub):
    p = sub.add_parser("report")
    p.add_argument("--json", required=True)
    p.add_argument("--out", default="")
    p = sub.add_parser("sweep")
    p.add_argument("--scenes", default="")
    p.add_argument("--modes", default="chromium,bro,bro-aot")
    p.add_argument("--out", default=str(rw.WORK / "timing"))
    p.add_argument("--rest", type=float, default=20.0)
    p.add_argument("--light-rest", type=float, default=5.0)
    p.add_argument("--reps", type=int, default=2)
    p.add_argument("--perf", type=int, default=1)
    p.add_argument("--merge", action="store_true", help="keep the other scenes of an existing timing.json")
    p.add_argument("--reuse", action="store_true",
                   help="with --merge: keep each scene's N values and other modes' points, run only --modes")
    p = sub.add_parser("time")
    p.add_argument("--browser", required=True, choices=list(MODES))
    p.add_argument("--scene", required=True)
    p.add_argument("--n", type=int, required=True)
    p.add_argument("--warm", type=int, default=120)
    p.add_argument("--meas", type=int, default=300)
    p.add_argument("--gpu", type=int, default=1)
    p.add_argument("--json", default="")
    p = sub.add_parser("perf")
    p.add_argument("--scene", required=True)
    p.add_argument("--n", type=int, required=True)
    p.add_argument("--aot", action="store_true")
    p.add_argument("--meas", type=int, default=600)
    p.add_argument("--out", default=str(rw.WORK / "perf"))
    p.add_argument("--json", default="")
    return {"time": cmd_time, "perf": cmd_perf, "sweep": cmd_sweep, "report": cmd_report}
