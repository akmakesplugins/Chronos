#!/usr/bin/env python3
"""Compare two corpus runs (scoreboards) against each other and against elastique.

Usage: compare_tags.py baseline fix1-stereo [--metric d_icpd_dev_rad]

Prints, per metric, how many cases each tag wins vs elastique, and the mean
absolute delta — so a fix's effect on every metric is visible at a glance
(including the ones it was NOT meant to change, which is where regressions hide).
"""

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

KEYS = ["d_env_crest_db", "d_gap_fill_db", "d_stereo_corr", "d_icpd_dev_rad",
        "d_am_mod", "d_warble_cents", "d_hf_frac_db", "d_side_mid_db",
        "d_rms_db", "onset_rate_err"]
# Metrics where a HIGHER delta is better; everything else is judged on |delta|.
HIGHER_BETTER = {"d_env_crest_db", "d_stereo_corr", "d_hf_frac_db"}
LOWER_BETTER = {"d_gap_fill_db", "d_icpd_dev_rad", "d_am_mod"}


def load(tag):
    path = os.path.join(HERE, "out", tag, "scoreboard.json")
    with open(path) as f:
        return json.load(f)


def wins(cv, ev, key):
    if cv is None or ev is None:
        return None
    if key in HIGHER_BETTER:
        return cv > ev
    if key in LOWER_BETTER:
        return cv < ev
    return abs(cv) < abs(ev)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tags", nargs="+")
    ap.add_argument("--cases", action="store_true", help="print per-case detail")
    args = ap.parse_args()

    boards = {t: load(t) for t in args.tags}
    common = set.intersection(*[set(b) for b in boards.values()])
    print(f"{len(common)} common cases: {', '.join(args.tags)}\n")

    hdr = f"{'metric':18s}" + "".join(f"{t[:14]:>16s}" for t in args.tags) + f"{'elastique':>11s}"
    print(hdr + "   (wins / mean|delta|; last col = elastique's own mean|delta|)")
    print("-" * len(hdr))
    for key in KEYS:
        cells = []
        for t in args.tags:
            b = boards[t]
            w = n = 0
            acc = []
            for case in common:
                cv = b[case]["chronos"].get(key)
                ev = b[case]["elastique"].get(key)
                r = wins(cv, ev, key)
                if r is not None:
                    n += 1
                    w += 1 if r else 0
                    acc.append(abs(cv))
            mean = sum(acc) / len(acc) if acc else float("nan")
            cells.append(f"{w:2d}/{n:<2d} {mean:7.3f}")
        # Elastique's own mean |delta| — the bar. When both sides are far below it
        # the win count is a coin flip on noise, so compare the means, not the wins.
        eacc = [abs(boards[args.tags[0]][c]["elastique"][key])
                for c in common if boards[args.tags[0]][c]["elastique"].get(key) is not None]
        emean = sum(eacc) / len(eacc) if eacc else float("nan")
        print(f"{key:18s}" + "".join(f"{c:>16s}" for c in cells) + f"{emean:11.3f}")

    # Engine routing changes.
    print("\nrouting:")
    for t in args.tags:
        counts = {}
        for case in common:
            e = boards[t][case]["engine"]
            counts[e] = counts.get(e, 0) + 1
        print(f"  {t:16s} " + ", ".join(f"{k}={v}" for k, v in sorted(counts.items())))

    if args.cases:
        print("\nper-case worst regressions (vs first tag):")
        base = args.tags[0]
        rows = []
        for case in sorted(common):
            for key in KEYS:
                a = boards[base][case]["chronos"].get(key)
                for t in args.tags[1:]:
                    b = boards[t][case]["chronos"].get(key)
                    if a is None or b is None:
                        continue
                    if abs(b) > abs(a) + 1e-9:
                        rows.append((abs(b) - abs(a), case, key, a, b, t))
        rows.sort(reverse=True)
        for d, case, key, a, b, t in rows[:20]:
            print(f"  {case:24s} {key:18s} {a:+8.3f} -> {b:+8.3f}  ({t})")


if __name__ == "__main__":
    main()
