#!/usr/bin/env python3
"""Run the loop corpus through chronos_file and elastique_file, analyze every
case, and write a scoreboard. This is the outer loop of the quality-feedback
cycle: change the engine, re-run, watch the deltas move.

Usage:  .venv/bin/python feedback/run_corpus.py [--tag baseline] [--quick]
                                                [--engine auto] [--loops N]
"""

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# Root of the loop corpus. The sample content is licensed and not distributed, so
# point this at your own: set CHRONOS_CORPUS, or pass --corpus. The CORPUS entries
# below are paths relative to it.
PACK = os.environ.get(
    "CHRONOS_CORPUS",
    "/Users/anantha/Documents/Sample Library/EDM/CymaticsEDMStarterPack-V2-j6e/"
    "Cymatics - EDM Starter Pack")

CORPUS = [
    ("drum_ghouse",    "Drums - Loops/Loops - Full/Cymatics - G House Full Drum Loop 7 - 126 BPM.wav"),
    ("drum_boombox",   "Drums - Loops/Loops - Full/Cymatics - Boombox Full Drum Loop 12 - 140 BPM.wav"),
    ("drum_dnb",       "Drums - Loops/Loops - Full/Cymatics - Drum & Bass Full Drum Loop 2 - 174 BPM.wav"),
    ("drum_shuffle",   "Drums - Loops/Loops - Full/Cymatics - Shuffle Full Drum Loop 2 - 128 BPM.wav"),
    ("drum_mc2_150F",  "Drums - Loops/Loops - Full/Cymatics - Master Collection Vol 2 Full Drum Loop 18 - 150 BPM F.wav"),
    ("brass_moon100",  "Instruments/Brass Loops/Cymatics - Moonlight Brass Loop 2 - 100 BPM A Min.wav"),
    ("guitar_chord",   "Instruments/Guitar Loops/Cymatics - Guitar Chord Loop 2 - 128 BPM Dmaj.wav"),
    ("guitar_melody",  "Instruments/Guitar Loops/Cymatics - Titan Guitar Melody Loop 1 - 140 BPM D Maj.wav"),
    ("piano_house",    "Instruments/Piano Loops/Cymatics - House Piano Loop 3 - 128 BPM F Min.wav"),
    ("piano_pop",      "Instruments/Piano Loops/Cymatics - Pop Piano 1 - 100 BPM D# Maj.wav"),
    ("strings_house",  "Instruments/String Loops/Cymatics - House String Loop 3 - 128 BPM A# Min.wav"),
    ("strings_moon",   "Instruments/String Loops/Cymatics - Moonlight String Loop 3 - 110 BPM G# Min.wav"),
]

CASES = [
    ("t1.50",  ["--time", "1.5"]),
    ("t2.00",  ["--time", "2.0"]),
    ("t0.80",  ["--time", "0.8"]),
    ("p+3",    ["--time", "1.0", "--pitch", "3"]),
]
QUICK_CASES = [("t1.50", ["--time", "1.5"]), ("t2.00", ["--time", "2.0"])]


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("FAIL:", " ".join(cmd), file=sys.stderr)
        print(r.stderr[-2000:], file=sys.stderr)
        return None
    return r.stdout.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", default="baseline")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--engine", default="auto")
    ap.add_argument("--loops", type=int, default=0, help="limit corpus size")
    ap.add_argument("--skip-elastique", action="store_true",
                    help="reuse existing elastique outputs (engine iteration mode)")
    ap.add_argument("--corpus", default=PACK,
                    help="root of the loop corpus (or set CHRONOS_CORPUS)")
    args = ap.parse_args()

    pack_root = args.corpus
    if not os.path.isdir(pack_root):
        print(f"corpus not found: {pack_root}\n"
              f"Set CHRONOS_CORPUS or pass --corpus. See qa/README.md.", file=sys.stderr)
        return 1

    outdir = os.path.join(HERE, "out", args.tag)
    eldir = os.path.join(HERE, "out", "elastique")     # reference cache, tag-independent
    os.makedirs(outdir, exist_ok=True)
    os.makedirs(eldir, exist_ok=True)

    chronos = os.path.join(ROOT, "build", "chronos_file")
    elastique = os.path.join(ROOT, "build", "elastique_file")
    cases = QUICK_CASES if args.quick else CASES
    corpus = CORPUS[: args.loops] if args.loops else CORPUS

    board = {}
    for label, rel in corpus:
        src = os.path.join(pack_root, rel)
        if not os.path.exists(src):
            print(f"missing: {src}", file=sys.stderr)
            continue
        for cname, cargs in cases:
            case = f"{label}_{cname}"
            chr_wav = os.path.join(outdir, case + "_chr.wav")
            el_wav = os.path.join(eldir, case + "_el.wav")

            info = run([chronos, src, chr_wav, *cargs,
                        "--engine", args.engine, "--quality", "best"])
            if info is None:
                continue
            engine = info.split()[0].split("=")[1] if "engine=" in info else "?"

            if not (args.skip_elastique and os.path.exists(el_wav)):
                if run([elastique, src, el_wav, *cargs]) is None:
                    continue

            tr = float(cargs[1])
            jso = os.path.join(outdir, case + ".json")
            png = os.path.join(outdir, case + ".png")
            run([sys.executable, os.path.join(HERE, "analyze.py"),
                 "--input", src,
                 "--version", "chronos", chr_wav,
                 "--version", "elastique", el_wav,
                 "--time-ratio", cargs[1],
                 "--json-out", jso, "--png-out", png])
            try:
                with open(jso) as f:
                    res = json.load(f)
                board[case] = {
                    "engine": engine,
                    "chronos": res["versions"]["chronos"]["deltas"],
                    "elastique": res["versions"]["elastique"]["deltas"],
                }
                print(f"{case:26s} [{engine:13s}] done")
            except Exception as e:
                print(f"{case}: analysis failed: {e}", file=sys.stderr)

    with open(os.path.join(outdir, "scoreboard.json"), "w") as f:
        json.dump(board, f, indent=1)

    # Markdown scoreboard: for the key metrics, chronos delta vs elastique delta.
    keys = ["d_env_crest_db", "d_gap_fill_db", "d_stereo_corr", "d_icpd_dev_rad",
            "d_am_mod", "d_warble_cents", "d_hf_frac_db", "onset_rate_err"]
    lines = ["| case | engine | " + " | ".join(k.replace("d_", "") for k in keys) + " |",
             "|---" * (len(keys) + 2) + "|"]
    wins = {k: [0, 0] for k in keys}   # [chronos better, elastique better]
    better_low = {"d_gap_fill_db", "d_icpd_dev_rad", "d_am_mod", "d_warble_cents"}
    for case in sorted(board):
        b = board[case]
        cells = []
        for k in keys:
            cv, ev = b["chronos"].get(k), b["elastique"].get(k)
            if cv is None or ev is None:
                cells.append("–")
                continue
            if k in better_low:
                cwin = abs(cv) < abs(ev) if k == "onset_rate_err" else cv < ev
            elif k in ("d_env_crest_db", "d_stereo_corr", "d_hf_frac_db"):
                cwin = cv > ev
            else:
                cwin = abs(cv) < abs(ev)
            wins[k][0 if cwin else 1] += 1
            mark = "**" if cwin else ""
            cells.append(f"{mark}{cv:+.2f}{mark} / {ev:+.2f}")
        lines.append(f"| {case} | {b['engine']} | " + " | ".join(cells) + " |")
    lines.append("")
    lines.append("| metric | chronos wins | elastique wins |")
    lines.append("|---|---|---|")
    for k in keys:
        lines.append(f"| {k} | {wins[k][0]} | {wins[k][1]} |")
    with open(os.path.join(outdir, "scoreboard.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\nwrote", os.path.join(outdir, "scoreboard.md"))
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
