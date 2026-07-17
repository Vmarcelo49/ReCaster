#!/usr/bin/env python3
"""Compare host and joiner RNG state logs frame-by-frame.

Reads host_debug.log and join_debug.log, extracts the RNG log lines
(format: "RNG F <tick> | idx=<i> frm=<f> | r0=0x... r1=0x... r2=0x... r3=... | tag=... rerun=... rngSync=..."),
and finds the FIRST frame where the RNG state diverges between the two
peers.

Usage:
    python3 compare_rng.py [--tag periodic] [--max-frames 300]

By default, compares the "periodic" tag (one reading per frame at the
end of frameStep). Use --tag to compare a different tag (e.g. post-load,
post-save).
"""
import re
import sys
import argparse
from pathlib import Path
from collections import defaultdict

LOG_DIR = Path("/home/marcelo/Downloads/Community_Edition_3-1-004/MBAACC - Community Edition/MBAACC/caster")
HLOG = LOG_DIR / "host_debug.log"
JLOG = LOG_DIR / "join_debug.log"

# Format: RNG F 123 | idx=4 frm=89 | r0=0x12345678 r1=0x9abcdef0 r2=0x13579bdf r3=deadbeefcafebabe... | tag=periodic rerun=0 rngSync=0
RNG_RE = re.compile(
    r"^RNG F (\d+) \| idx=(\d+) frm=(\d+) \| "
    r"r0=(0x[0-9a-f]+) r1=(0x[0-9a-f]+) r2=(0x[0-9a-f]+) r3=([0-9a-f]+) \| "
    r"tag=(\w+) rerun=(\d) rngSync=(\d)"
)

def parse_log(path):
    """Return dict: {(idx, frm): {tag: {r0, r1, r2, r3, rerun, rngSync, tick}}}"""
    out = defaultdict(dict)
    if not path.exists():
        return out
    for line in path.read_text().splitlines():
        m = RNG_RE.match(line)
        if not m:
            continue
        (tick, idx, frm, r0, r1, r2, r3, tag, rerun, rng_sync) = m.groups()
        out[(int(idx), int(frm))][tag] = {
            "tick": int(tick),
            "idx": int(idx),
            "frm": int(frm),
            "r0": r0,
            "r1": r1,
            "r2": r2,
            "r3": r3,
            "rerun": int(rerun),
            "rngSync": int(rng_sync),
        }
    return out

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", default="periodic",
                        help="Which tag to compare (periodic, post-load, post-save, etc.)")
    parser.add_argument("--idx", type=int, default=4,
                        help="Only compare entries with this transition index (default: 4=InGame)")
    parser.add_argument("--max-frames", type=int, default=500,
                        help="Max frames to compare")
    parser.add_argument("--context", type=int, default=5,
                        help="Number of frames to show before/after divergence")
    args = parser.parse_args()

    host = parse_log(HLOG)
    join = parse_log(JLOG)

    # Filter to only the requested idx
    host_filt = {k: v for k, v in host.items() if k[0] == args.idx}
    join_filt = {k: v for k, v in join.items() if k[0] == args.idx}
    all_keys = sorted(set(host_filt.keys()) | set(join_filt.keys()))
    all_frames = [k[1] for k in all_keys]

    print(f"Comparing tag='{args.tag}' idx={args.idx} across {len(all_frames)} frames")
    print(f"Host log: {HLOG}")
    print(f"Join log: {JLOG}")
    print()

    # Find first divergence
    divergence_frame = None
    for frm in all_frames:
        key = (args.idx, frm)
        h = host_filt.get(key, {}).get(args.tag)
        j = join_filt.get(key, {}).get(args.tag)
        if not h or not j:
            continue
        if h["r0"] != j["r0"] or h["r1"] != j["r1"] or h["r2"] != j["r2"] or h["r3"] != j["r3"]:
            divergence_frame = frm
            break

    if divergence_frame is None:
        print(f"NO RNG divergence found in tag='{args.tag}' idx={args.idx} across {len(all_frames)} frames!")
        print("Both peers have identical RNG state. The desync must be in non-RNG state.")
        return

    print(f"*** FIRST RNG DIVERGENCE at idx={args.idx} frame {divergence_frame} (tag={args.tag}) ***")
    print()

    # Show context: N frames before and after
    context_start = max(0, divergence_frame - args.context)
    context_end = divergence_frame + args.context

    print(f"{'frm':>4} | {'host r0':>12} {'host r1':>12} {'host r2':>12} {'host r3[0:16]':>18} | {'join r0':>12} {'join r1':>12} {'join r2':>12} {'join r3[0:16]':>18} | {'match':>5} {'rerun(h/j)':>10} {'sync(h/j)':>10}")
    print("-" * 150)
    for frm in range(context_start, context_end + 1):
        key = (args.idx, frm)
        h = host_filt.get(key, {}).get(args.tag)
        j = join_filt.get(key, {}).get(args.tag)
        if not h and not j:
            continue
        if not h:
            print(f"{frm:>4} | {'(missing)':>12} {'':>12} {'':>12} {'':>18} | {j['r0']:>12} {j['r1']:>12} {j['r2']:>12} {j['r3'][:16]:>18} | {'???':>5} {'?/'+str(j['rerun']):>10} {'?/'+str(j['rngSync']):>10}")
            continue
        if not j:
            print(f"{frm:>4} | {h['r0']:>12} {h['r1']:>12} {h['r2']:>12} {h['r3'][:16]:>18} | {'(missing)':>12} {'':>12} {'':>12} {'':>18} | {'???':>5} {str(h['rerun'])+'/?':>10} {str(h['rngSync'])+'/?':>10}")
            continue
        match = (h["r0"] == j["r0"] and h["r1"] == j["r1"] and h["r2"] == j["r2"] and h["r3"] == j["r3"])
        marker = "OK" if match else "** DIFF **"
        print(f"{frm:>4} | {h['r0']:>12} {h['r1']:>12} {h['r2']:>12} {h['r3'][:16]:>18} | {j['r0']:>12} {j['r1']:>12} {j['r2']:>12} {j['r3'][:16]:>18} | {marker:>5} {str(h['rerun'])+'/'+str(j['rerun']):>10} {str(h['rngSync'])+'/'+str(j['rngSync']):>10}")

    print()
    print(f"Divergence details at idx={args.idx} frame {divergence_frame}:")
    key = (args.idx, divergence_frame)
    h = host_filt[key][args.tag]
    j = join_filt[key][args.tag]
    for field in ["r0", "r1", "r2", "r3"]:
        if h[field] != j[field]:
            print(f"  {field}: host={h[field]} join={j[field]}")
    print(f"  host rerun={h['rerun']} rngSync={h['rngSync']}")
    print(f"  join rerun={j['rerun']} rngSync={j['rngSync']}")

    # Also check if there were any rollbacks around the divergence
    print()
    print(f"Rollback events around idx={args.idx} frame {divergence_frame} (±{args.context}):")
    for label, log_path in [("host", HLOG), ("join", JLOG)]:
        if not log_path.exists():
            continue
        rb_events = []
        for line in log_path.read_text().splitlines():
            if not line.startswith("EVENT rollback-trigger"):
                continue
            m = re.search(r"target_frm (\d+)", line)
            if not m:
                continue
            target_frm = int(m.group(1))
            if context_start <= target_frm <= context_end:
                rb_events.append(line)
        print(f"  {label}:")
        for e in rb_events:
            print(f"    {e}")

if __name__ == "__main__":
    main()
