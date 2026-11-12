#!/usr/bin/env python3
# fsr-trace.py <ffx_xefg_trace.txt> [from_s to_s] [--frames=N] [--dump] [--subs] [--min-world-ms=M] [--max-world-ms=M]
# Reads the event file of FFX_XEFG_TRACE=1 (ffxfg/ffx_trace.h, a debug build of the frame generation DLL): the time line
# of every frame on the FSR routes. Prints, for the frames with an FFX prepare (frame generation running), and
# separately for the stretches with the low latency's wait on and off (FFX_XEFG_AB):
#   frame time and its percentiles, the time in Present, our wait, how long frames were held up in front of Present,
#   when the GPU reached the frame's Present, the real swap chain's presents (the frames as they go to the display),
#   where each thread calls the message / input APIs, and the latency figures:
#     frame start (Present returned to the game) -> end of the frame's present
#     input read (XInputGetState of each thread that calls it every frame) -> end of the frame's present, -> the GPU
#     has finished the frame's interpolation (its first image can be shown from then on), -> the frame's first and
#     last real present returned
# and, with FFX_XEFG_AB, the frame time of every turn and per setting the mean of its turns: what the wait costs in
# frame rate shows between neighbouring turns, a scene drifts more than that over a run.
# --subs (a trace made with FFX_XEFG_TRACE_SUBS=1 too): every submission to a D3D12 queue with the time the GPU
# finished it: per queue the time of a frame with unfinished work, and --frames=N frames with all their submissions.
# --frames=N lists N frames from the middle with all their events; --dump prints the events as they are;
# --min-world-ms=M leaves out frames shorter than M ms (menus) from the statistics, --max-world-ms=M frames longer
# than M ms (loading, hitches) from the turns' means. from_s to_s: only that part of the file (seconds).
import collections
import statistics
import sys

NAMES = ("? GP_CALL PX_CALL PX_RET GP_RET RP_CALL RP_RET ECL SIG FWAIT PREP CONF GPU1 GPU2 INPUT XSLEEP NOTE WAITOBJ "
         "SCCALL").split()
APIS = ["PeekMessageW", "PeekMessageA", "GetMessageW", "GetMessageA", "XInput(1_4)", "XInput(1_3)", "XInput(9_1_0)",
        "GetAsyncKeyState", "GetKeyState", "GetKeyboardState", "GetCursorPos", "GetRawInputData", "GetRawInputBuffer",
        "XInputEx(1_4)", "XInputEx(1_3)", "XInput(uap)"]
XINPUT = {4, 5, 6, 13, 14, 15}
SC = ["?", "GetBuffer", "GetCurrentBackBufferIndex", "ResizeBuffers", "GetDesc", "GetFrameStatistics", "CheckColorSpaceSupport", "GetContainingOutput", "GetFullscreenState", "GetDesc1", "GetFullscreenDesc", "GetLastPresentCount", "GetHwnd"]


def load(path, t0=None, t1=None):
    ev = []
    for line in open(path):
        p = line.split()
        if len(p) != 6:
            continue
        t = int(p[0]) / 1000.0
        if t0 is not None and t < t0 * 1000:
            continue
        if t1 is not None and t > t1 * 1000:
            break
        ev.append((t, int(p[1]), int(p[2]), int(p[3]), int(p[4]), int(p[5])))
    return ev


def pct(v, q):
    if not v:
        return float("nan")
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * q))]


def stat(name, v):
    if not v:
        print(f"  {name:52s} (none)")
        return
    print(f"  {name:52s} mean {statistics.fmean(v):7.2f}  p5 {pct(v, .05):7.2f}  p50 {pct(v, .5):7.2f}  p95 {pct(v, .95):7.2f}"
          f"  p99 {pct(v, .99):7.2f}  max {max(v):7.2f}  n {len(v)}")


def new_frame(n, hold_on, since):
    return {"n": n, "ecl": [], "ecl_in": [], "inp": [], "rp": [], "sig": [], "fw": [], "sc": [], "hold_on": hold_on,
            "since": since}


def subs(path, t_from, t_to, nshow, lo, hi):
    """FFX_XEFG_TRACE_SUBS=1: every submission to a D3D12 queue with the time the GPU finished it: the queues, per queue
    the time of a frame in which it had unfinished work, and nshow frames from the middle with all their submissions"""
    queues = {}
    subs = []          # [submit t, tid, queue, value, lists, flags, done t or None]
    bykey = {}
    frames = []        # [call, pxc, take, ret, n]
    rp = []            # real presents (call, ret, tid)
    rp_open = {}
    gpu = {12: {}, 13: {}, 19: {}}
    pt = None
    cur = None
    for line in open(path):
        p = line.split()
        if len(p) != 6:
            continue
        t = int(p[0]) / 1000.0
        if t_from is not None and t < t_from:
            continue
        if t_to is not None and t > t_to:
            break
        tid, ty, n, a, b = int(p[1]), int(p[2]), int(p[3]), int(p[4]), int(p[5])
        if ty == 21:
            queues[n] = (a, b)
        elif ty == 7:
            v, q = b >> 8, (b >> 4) & 15
            if v:
                s = [t, tid, q, v, a, b & 15, None]
                subs.append(s)
                bykey[(q, v)] = s
        elif ty == 20:
            # every value up to a of queue b is finished now
            v = a
            while v > 0 and (b, v) in bykey and bykey[(b, v)][6] is None:
                bykey[(b, v)][6] = t
                v -= 1
        elif ty == 1:
            pt = tid
            cur = [t, None, None, None, b]
        elif ty == 2 and cur:
            cur[1] = t
        elif ty == 3 and cur:
            cur[2] = t
        elif ty == 4 and cur and tid == pt:
            cur[3] = t
            frames.append(cur)
            cur = None
        elif ty == 5:
            rp_open[tid] = t
        elif ty == 6 and tid in rp_open:
            rp.append((rp_open.pop(tid), t, tid))
        elif ty in gpu:
            gpu[ty][a] = t

    TYPES = {0: "direct", 1: "bundle", 2: "compute", 3: "copy"}
    print("queues:", ", ".join(f"q{i} {TYPES.get(ty, ty)} {ptr:x}" for i, (ptr, ty) in sorted(queues.items())))
    good = [(a, b) for a, b in zip(frames, frames[1:]) if lo <= b[2] - a[2] <= hi]
    print(f"{len(frames)} frames, {len(good)} of {lo}..{hi} ms, {len(subs)} submissions")
    if not good:
        return

    # per queue: the time per frame with unfinished work on it (from take to take)
    busy = collections.defaultdict(list)
    T = []
    subs.sort(key=lambda s: s[0])
    si = 0
    for a, b in good:
        t0, t1 = a[2], b[2]
        T.append(t1 - t0)
        per = collections.defaultdict(list)
        for s in subs:
            if s[6] is None or s[6] < t0 - 100:
                continue
            if s[0] > t1:
                break
            per[s[2]].append((max(s[0], t0), min(s[6], t1)))
        for q, iv in per.items():
            iv = sorted(x for x in iv if x[1] > x[0])
            tot, end = 0.0, None
            for x0, x1 in iv:
                if end is None or x0 > end:
                    tot += x1 - x0
                    end = x1
                elif x1 > end:
                    tot += x1 - end
                    end = x1
            busy[q].append(tot)
    print(f"frame time mean {statistics.fmean(T):.2f} ms")
    for q in sorted(busy):
        v = busy[q] + [0.0] * (len(good) - len(busy[q]))
        print(f"  q{q} ({TYPES.get(queues.get(q, (0, -1))[1], '?')}): unfinished work for {statistics.fmean(v):6.2f} ms of a frame ({100 * statistics.fmean(v) / statistics.fmean(T):.0f}%)")

    mid = len(good) // 2
    for a, b in good[mid:mid + nshow]:
        t0, t1 = a[2], b[2]
        print(f"\nframe with present {b[4]}: take {t0:.3f} -> take {t1:.3f} ({t1 - t0:.2f} ms); times from the first take")
        rows = [(b[0] - t0, "PRESENT CALLED by the game"), (b[1] - t0, "proxy Present called"), (0.0, "take (proxy Present returned)"),
                (t1 - t0, "TAKE (proxy Present returned)")]
        for c, r, tid in rp:
            if t0 - 1 <= c <= t1 + 1:
                rows.append((c - t0, f"real present by t{tid}, {r - c:.2f} ms inside"))
        for ty, nm in ((12, "GPU reached the Present call of"), (13, "GPU finished the interpolation of"), (19, "GPU got to the first work of")):
            for k, t in gpu[ty].items():
                if t0 <= t <= t1:
                    rows.append((t - t0, f"{nm} present {k}"))
        lastdone = {}
        for s in subs:
            if s[0] < t0 - 40:
                continue
            if s[0] > t1 + 1:
                break
            d = s[6]
            prev = lastdone.get(s[2])
            ran = (d - max(s[0], prev)) if d is not None and prev is not None else (d - s[0] if d is not None else None)
            if d is not None:
                lastdone[s[2]] = d
            if s[0] >= t0 - 1 or (d is not None and d >= t0):
                rows.append((s[0] - t0, f"submit t{s[1]} q{s[2]}#{s[3]} {s[4]} lists{' (own)' if s[5] & 2 else ''} -> finished at "
                             + (f"{d - t0:7.2f} ({ran:5.2f} ms after the one before / its submission)" if d is not None else "?")))
        for t, txt in sorted(rows):
            print(f"  {t:8.2f}  {txt}")



def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = [a for a in sys.argv[1:] if a.startswith("--")]
    t0 = float(args[1]) if len(args) > 1 else None
    t1 = float(args[2]) if len(args) > 2 else None
    ev = load(args[0], t0, t1)
    if "--dump" in opts:
        for t, tid, ty, n, a, b in ev:
            extra = APIS[a] if ty == 14 and a < len(APIS) else ""
            print(f"{t:11.3f} t{tid:<5d} {NAMES[ty]:8s} n={n:<5d} a={a:<12d} b={b} {extra}")
        return
    minw = next((float(a.split("=")[1]) for a in opts if a.startswith("--min-world-ms=")), 0.0)
    maxw = next((float(a.split("=")[1]) for a in opts if a.startswith("--max-world-ms=")), 1e9)
    if "--subs" in opts:
        k = next((int(a.split("=")[1]) for a in opts if a.startswith("--frames=")), 2)
        subs(args[0], t0 * 1000 if t0 is not None else None, t1 * 1000 if t1 is not None else None, k, minw or 20.0,
             maxw if maxw < 1e9 else 80.0)
        return
    F = collections.OrderedDict()
    cur = None            # the present under way on the present thread
    last_ret = None       # (n, time) of the newest GP_RET
    pres_tid = None
    hold_on, switch_t = 0, None
    rp, rp_open = [], {}
    G3 = {}
    for t, tid, ty, n, a, b in ev:
        if ty == 1:
            pres_tid = tid
            f = F.get(b) or F.setdefault(b, new_frame(b, hold_on, t - switch_t if switch_t else 1e9))
            f["gp_call"] = t
            f["id"] = a
            f["hold_on"] = hold_on
            f["since"] = t - switch_t if switch_t else 1e9
            cur = f
            if last_ret:
                f["prev_ret"] = last_ret[1]
        elif ty == 2 and cur:
            cur["px_call"] = t
        elif ty == 3 and cur:
            cur["px_ret"] = t
        elif ty == 4 and cur:
            cur["gp_ret"] = t
            cur["after"] = a / 1000.0     # proxy return -> return to the game: our wait and XeLL's sleep
            cur["block"] = n / 100.0      # held up in front of the present, as the DLL measured it
            last_ret = (cur["n"], t)
            cur = None
        elif ty == 12 or ty == 13:
            key = "gpu1" if ty == 12 else "gpu2"
            for k in list(F.keys())[-8:]:
                if k <= a and key not in F[k]:
                    F[k][key] = t
        elif ty == 19:
            G3[a] = t     # the queue and the GPU reached the first submission of the frame with present number a
        elif ty == 16 and a == 1:
            hold_on, switch_t = b, t
        elif ty == 5:
            rp_open[tid] = t
        elif ty == 6:
            if tid in rp_open:
                rp.append((rp_open.pop(tid), t, tid))
        else:
            nxt = (last_ret[0] + 1) if last_ret else None
            tgt = cur if (cur and tid == pres_tid) else None
            if tgt is None and nxt is not None:
                tgt = F.get(nxt) or F.setdefault(nxt, new_frame(nxt, hold_on, 1e9))
            if tgt is None:
                continue
            if ty == 7:
                (tgt["ecl_in"] if (cur and tid == pres_tid) else tgt["ecl"]).append((t, tid, a, b, n))
            elif ty == 10:
                tgt.setdefault("prep", t)
                tgt.setdefault("prep_tid", tid)
            elif ty == 11:
                tgt.setdefault("conf", t)
            elif ty == 14:
                tgt["inp"].append((t, tid, a, b))
            elif ty == 8:
                tgt["sig"].append((t, tid, a, b))
            elif ty == 9:
                tgt["fw"].append((t, tid, a, b, n))
            elif ty == 18:
                tgt["sc"].append((t, tid, a, b, n))
    frames = [f for f in F.values() if "gp_call" in f and "gp_ret" in f and "prev_ret" in f and "px_ret" in f]
    print(f"{len(ev)} events, {len(frames)} frames, {len(rp)} real presents, present thread t{pres_tid}")
    if not frames:
        return
    # the real presents of a frame: the one the proxy makes on the game's present thread inside its Present (the
    # frame's first image) and those of XeSS-FG's presentation thread after it, up to the next frame's first. (By
    # time alone they cannot be told apart: while a frame waits in Present the presentation thread is still
    # presenting the frame before.)
    byc = sorted(frames, key=lambda f: f["px_call"])
    j, curf = -1, None
    for c, r, tid in sorted(rp):
        if tid == pres_tid:
            while j + 1 < len(byc) and byc[j + 1]["px_call"] <= c:
                j += 1
            curf = byc[j] if j >= 0 and byc[j]["px_call"] <= c <= byc[j]["px_ret"] + 0.5 else None
        if curf is not None:
            curf["rp"].append((c, r, tid))
    for a, b in zip(frames, frames[1:]):
        if b["n"] == a["n"] + 1:
            b["T"] = b["px_ret"] - a["px_ret"]      # present to present
            a["next"] = b

    def summarize(fr, title):
        fr = [f for f in fr if f.get("T", 0) >= minw]
        print(f"\n== {title}: {len(fr)} frames")
        if len(fr) < 5:
            return
        T = [f["T"] for f in fr if "T" in f]
        stat("frame time (present end to present end)", T)
        if T:
            print(f"      = {1000 / statistics.fmean(T):.1f} fps rendered")
        stat("in Present (the proxy's)", [f["px_ret"] - f["px_call"] for f in fr])
        stat("held up in front of the present (DLL's figure)", [f["block"] for f in fr])
        stat("after the proxy's Present: our wait + XeLL's sleep", [f["after"] for f in fr])
        stat("Present returned -> next Present call", [f["gp_call"] - f["prev_ret"] for f in fr])
        stat("LATENCY frame start -> end of its present", [f["px_ret"] - f["prev_ret"] for f in fr])
        stat("Present call -> GPU reached it", [f["gpu1"] - f["gp_call"] for f in fr if "gpu1" in f])
        stat("proxy return -> GPU finished the interpolation", [f["gpu2"] - f["px_ret"] for f in fr if "gpu2" in f])
        # the frame's first work: the game's first submission on the swap chain's queue (the DLL's figure is this one;
        # Resident Evil Requiem submits one list on another queue at the start of each frame, the rest at its Present)
        def first(f):
            return next((e[0] for e in f["ecl"] if e[3] & 1 and not e[3] & 2), None)

        stat("first work -> GPU finished the frame before (ahead)",
             [p["gpu2"] - first(f) for f in fr for p in [F.get(f["n"] - 1)] if first(f) and p and "gpu2" in p])
        stat("previous frame's interpolation done -> Present call",
             [f["gp_call"] - p["gpu2"] for f in fr for p in [F.get(f["n"] - 1)] if p and "gpu2" in p])
        stat("first work -> queue and GPU got to it", [G3[f["n"]] - first(f) for f in fr if first(f) and f["n"] in G3])
        stat("GPU finished the frame before -> got to first work",
             [G3[f["n"]] - p["gpu2"] for f in fr for p in [F.get(f["n"] - 1)] if f["n"] in G3 and p and "gpu2" in p])
        stat("frame start -> first work (the game's queue)", [first(f) - f["prev_ret"] for f in fr if first(f)])
        stat("first work -> Present call", [f["gp_call"] - first(f) for f in fr if first(f)])
        stat("first submission on any queue -> Present call", [f["gp_call"] - f["ecl"][0][0] for f in fr if f["ecl"]])
        stat("GetFrameStatistics from the swap chain: ms inside", [e[4] / 1000.0 for f in fr for e in f["sc"] if e[2] == 5 and e[3] == 0])
        # the calls of the message / input APIs
        by = collections.defaultdict(list)
        for f in fr:
            seen = set()
            for t, tid, a, b in f["inp"]:
                if (a, tid) in seen or t > f["gp_call"]:
                    continue
                seen.add((a, tid))
                by[(a, tid)].append((f, t))
        print("  message / input calls (first per thread after the frame's start): ms after the start | before the Present call")
        for (a, tid), v in sorted(by.items(), key=lambda kv: -len(kv[1])):
            if len(v) < len(fr) * 0.5:
                continue
            nm = APIS[a] if a < len(APIS) else str(a)
            aft = [t - f["prev_ret"] for f, t in v]
            bef = [f["gp_call"] - t for f, t in v]
            print(f"    {nm:16s} t{tid:<5d} in {100 * len(v) / len(fr):5.1f}% of frames: after start p50 {pct(aft, .5):6.2f}"
                  f" (p5 {pct(aft, .05):6.2f} p95 {pct(aft, .95):6.2f}) | before Present p50 {pct(bef, .5):6.2f}")
            if a in XINPUT and len(v) >= len(fr) * 0.9:
                stat(f"LATENCY input t{tid} -> end of its present", [f["px_ret"] - t for f, t in v])
                stat(f"LATENCY input t{tid} -> GPU finished the interpolation", [f["gpu2"] - t for f, t in v if "gpu2" in f])
                stat(f"LATENCY input t{tid} -> the frame's first image presented", [f["rp"][0][1] - t for f, t in v if len(f["rp"]) >= 2])
                stat(f"LATENCY input t{tid} -> the frame's last image presented", [f["rp"][-1][1] - t for f, t in v if len(f["rp"]) >= 2])
        nrp = [len(f["rp"]) for f in fr]
        if sum(nrp):
            print(f"  real presents per frame: {collections.Counter(nrp).most_common(3)}")
            k = max(set(nrp), key=nrp.count)
            for i in range(k):
                d = [f["rp"][i][1] - f["rp"][i][0] for f in fr if len(f["rp"]) == k]
                c = [f["rp"][i][0] - f["px_ret"] for f in fr if len(f["rp"]) == k]
                print(f"    real present #{i + 1}: called {statistics.fmean(c):6.2f} ms after the proxy's return (p95 {pct(c, .95):6.2f}),"
                      f" inside {statistics.fmean(d):6.2f} ms (p50 {pct(d, .5):6.2f} p95 {pct(d, .95):6.2f})")
            # when the images are through: a real present returns when the one two before it is done (two may be under way)
            stat("the proxy's return -> the frame's last image presented", [f["rp"][-1][1] - f["px_ret"] for f in fr if len(f["rp"]) == k])
            ends = sorted(r for f in fr for c, r, tid in f["rp"])
            stat("interval between the real presents' returns", [b - a for a, b in zip(ends, ends[1:]) if b - a < 500])

    world = [f for f in frames if "prep" in f]
    summarize(world, "frames with an FFX prepare")
    states = sorted(set(f["hold_on"] for f in world))
    label = {0: "AS BEFORE: statistics from the swap chain, no wait", 1: "STATISTICS WITHOUT A WAIT, no wait of ours",
             2: "LOW LATENCY: statistics without a wait + our wait"}
    if len(states) > 1:
        for st in states:
            summarize([f for f in world if f["hold_on"] == st and f["since"] > 2000],
                      f"{label.get(st, st)} (from 2 s after each switch on)")
        # the turns: a new one where the setting changes
        turns = []
        for f in world:
            if not turns or turns[-1][0] != f["hold_on"] or f["since"] < turns[-1][3]:
                turns.append([f["hold_on"], f["px_ret"], [], f["since"]])
            turns[-1][3] = f["since"]
            if f["since"] > 2000 and minw <= f.get("T", 0) <= maxw:
                turns[-1][2].append(f)
        short = {0: "before", 1: "answers", 2: "low latency"}
        per = collections.defaultdict(list)
        print("\n== the turns (frames from 2 s after each switch on)")
        for st, at, fr, _ in turns:
            if len(fr) < 100:
                continue
            T = [f["T"] for f in fr]
            per[st].append(statistics.fmean(T))
            print(f"  at {at / 1000:7.1f} s  {short.get(st, st):12s} {len(fr):4d} frames  frame mean {statistics.fmean(T):6.2f}"
                  f" p50 {pct(T, .5):6.2f} p95 {pct(T, .95):6.2f}  our wait {statistics.fmean(f['after'] for f in fr):5.2f}"
                  f"  held up {statistics.fmean(f['block'] for f in fr):5.2f}")
        for st, v in sorted(per.items()):
            print(f"  {short.get(st, st):12s}: {len(v)} turns, mean of their frame times {statistics.fmean(v):6.2f} ms"
                  f" (min {min(v):6.2f}, max {max(v):6.2f})")
    for a in opts:
        if a.startswith("--frames="):
            k = int(a.split("=")[1])
            sel = [f for f in world if f.get("T", 0) >= minw]
            mid = len(sel) // 2
            for f in sel[mid:mid + k]:
                z = f["prev_ret"]
                print(f"\nframe n={f['n']} (0 = the previous Present returned, at {z:.3f} ms; wait {'on' if f['hold_on'] else 'off'})")
                rows = [(f["gp_call"], "Present called"), (f["px_call"], "proxy Present called"),
                        (f["px_ret"], f"proxy Present returned (held up {f['block']:.2f} ms)"),
                        (f["gp_ret"], f"Present returns to the game ({f['after']:.2f} ms after the proxy's)")]
                if "gpu1" in f:
                    rows.append((f["gpu1"], "GPU reached this Present call"))
                if "gpu2" in f:
                    rows.append((f["gpu2"], "GPU finished the interpolation"))
                if "prep" in f:
                    rows.append((f["prep"], f"FFX prepare t{f.get('prep_tid')}"))
                if "conf" in f:
                    rows.append((f["conf"], "FFX configure"))
                for t, tid, a, b, n in f["ecl"]:
                    rows.append((t, f"submit t{tid} {a} lists, {'game queue' if b & 1 else 'other queue'}"))
                for t, tid, a, b, n in f["ecl_in"]:
                    rows.append((t, f"  submit in Present t{tid} {a} lists, {'game queue' if b & 1 else 'other queue'}"))
                for t, tid, a, b in f["inp"]:
                    rows.append((t, f"input t{tid} {APIS[a] if a < len(APIS) else a} (x{b})"))
                for t, tid, a, b, n in f["fw"]:
                    rows.append((t, f"fence wait t{tid} fence {a:x} value {b} {'event' if n == 1 else str(n) + ' us inside'}"))
                for c, r, tid in f["rp"]:
                    rows.append((c, f"REAL PRESENT t{tid} ({r - c:.2f} ms inside)"))
                for t, tid, a, b, n in f["sc"]:
                    rows.append((t, f"swap chain t{tid} {SC[a] if a < len(SC) else a}({b}) {n} us inside"))
                for t, s in sorted(rows):
                    print(f"  {t - z:8.3f}  {s}")


main()
