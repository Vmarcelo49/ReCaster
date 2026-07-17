#!/usr/bin/env python3
"""Show inputs + rollback events + state around a specific frame.

Helps diagnose why the rollback isn't correcting P2's state.
"""
import re
import sys
from pathlib import Path

LOG_DIR = Path("/home/marcelo/Downloads/Community_Edition_3-1-004/MBAACC - Community Edition/MBAACC/caster")
HLOG = LOG_DIR / "host_debug.log"
JLOG = LOG_DIR / "join_debug.log"

# F <tick> | st=InGame idx=4 frm=149 | rmt:idx=4 frm=148 lcf=4/146 | spin:pass | rb:save | in:0x0089 0x0206
FRAME_RE = re.compile(
    r"^(?:i\d+ )?F (\d+) \| st=(\w+) idx=(\d+) frm=(\d+) \| rmt:idx=(\d+) frm=(\d+) lcf=(\d+)/(\d+) \| (spin:\S+) \| rb:(\w+) \| in:(0x[0-9a-f]+) (0x[0-9a-f]+)"
)

def parse_frames(path):
    """Return list of dicts for all frame lines."""
    out = []
    if not path.exists():
        return out
    for line in path.read_text().splitlines():
        m = FRAME_RE.match(line)
        if not m:
            continue
        (tick, st, idx, frm, rmt_idx, rmt_frm, lcf_idx, lcf_frm, spin, rb, local, remote) = m.groups()
        out.append({
            "tick": int(tick),
            "st": st,
            "idx": int(idx),
            "frm": int(frm),
            "rmt_idx": int(rmt_idx),
            "rmt_frm": int(rmt_frm),
            "lcf_idx": int(lcf_idx),
            "lcf_frm": int(lcf_frm),
            "spin": spin,
            "rb": rb,
            "local": local,
            "remote": remote,
        })
    return out

def parse_rollbacks(path):
    """Return list of (target_frm, cur_frm) for rollback-trigger events."""
    out = []
    if not path.exists():
        return out
    for line in path.read_text().splitlines():
        if not line.startswith("EVENT rollback-trigger"):
            continue
        m = re.search(r"target_frm (\d+).*cur_frm (\d+)", line)
        if m:
            out.append((int(m.group(1)), int(m.group(2))))
    return out

def main():
    target_frame = int(sys.argv[1]) if len(sys.argv) > 1 else 149
    context = int(sys.argv[2]) if len(sys.argv) > 2 else 15
    idx_filter = 4

    host_frames = [f for f in parse_frames(HLOG) if f["idx"] == idx_filter and f["st"] == "InGame"]
    join_frames = [f for f in parse_frames(JLOG) if f["idx"] == idx_filter and f["st"] == "InGame"]
    host_rb = parse_rollbacks(HLOG)
    join_rb = parse_rollbacks(JLOG)

    # Build dicts by frame
    host_by_frm = {f["frm"]: f for f in host_frames}
    join_by_frm = {f["frm"]: f for f in join_frames}

    start = max(0, target_frame - context)
    end = target_frame + 5

    print(f"=== Frames {start}-{end} (idx={idx_filter}) ===")
    print()
    print(f"{'frm':>4} | {'host local':>10} {'host rmt':>10} {'host lcf':>12} {'host rb':>6} | {'join local':>10} {'join rmt':>10} {'join lcf':>12} {'join rb':>6} | {'input_match':>11}")
    print("-" * 120)
    for frm in range(start, end + 1):
        h = host_by_frm.get(frm)
        j = join_by_frm.get(frm)
        if not h and not j:
            continue
        if not h:
            print(f"{frm:>4} | {'(missing)':>10} {'':>10} {'':>12} {'':>6} | {j['local']:>10} {j['remote']:>10} {j['lcf_idx']}/{j['lcf_frm']:>8} {j['rb']:>6} | {'???':>11}")
            continue
        if not j:
            print(f"{frm:>4} | {h['local']:>10} {h['remote']:>10} {h['lcf_idx']}/{h['lcf_frm']:>8} {h['rb']:>6} | {'(missing)':>10} {'':>10} {'':>12} {'':>6} | {'???':>11}")
            continue
        # Input match: host.local should == join.remote, host.remote == join.local
        # (host's local = P1, host's remote = P2 = join's local)
        input_match = (h["local"] == j["remote"] and h["remote"] == j["local"])
        marker = "OK" if input_match else "** DIFF **"
        print(f"{frm:>4} | {h['local']:>10} {h['remote']:>10} {h['lcf_idx']}/{h['lcf_frm']:>8} {h['rb']:>6} | {j['local']:>10} {j['remote']:>10} {j['lcf_idx']}/{j['lcf_frm']:>8} {j['rb']:>6} | {marker:>11}")

    print()
    print(f"=== Rollback events targeting frames {start}-{end} ===")
    print(f"  host: {[(t,c) for t,c in host_rb if start <= t <= end]}")
    print(f"  join: {[(t,c) for t,c in join_rb if start <= t <= end]}")

    # Show the last 10 rollbacks before the desync
    print()
    print(f"=== Last 10 rollbacks before frame {target_frame} ===")
    host_before = [(t,c) for t,c in host_rb if t < target_frame][-10:]
    join_before = [(t,c) for t,c in join_rb if t < target_frame][-10:]
    print(f"  host (target→cur): {host_before}")
    print(f"  join (target→cur): {join_before}")

if __name__ == "__main__":
    main()
