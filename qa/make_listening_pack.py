#!/usr/bin/env python3
"""Build an ear-test pack: for each loop, the input plus BEFORE / AFTER / élastique
renders of the same case, named so they sort together in any file browser.

The metrics say what changed; this is how you decide whether you like it.

Usage:
  .venv/bin/python feedback/make_listening_pack.py [--out DIR] [--ratio 2.0]
"""

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
# Root of the loop corpus — licensed content, not distributed. Point this at your
# own via CHRONOS_CORPUS or --corpus.
PACK = os.environ.get(
    "CHRONOS_CORPUS",
    "/Users/anantha/Documents/Sample Library/EDM/CymaticsEDMStarterPack-V2-j6e/"
    "Cymatics - EDM Starter Pack")

# A spread across the failure modes: dense kit, sparse kit, wide tonal pads,
# percussive-tonal hybrids.
LOOPS = [
    ("01_drums_ghouse",  "Drums - Loops/Loops - Full/Cymatics - G House Full Drum Loop 7 - 126 BPM.wav"),
    ("02_drums_dnb",     "Drums - Loops/Loops - Full/Cymatics - Drum & Bass Full Drum Loop 2 - 174 BPM.wav"),
    ("03_drums_boombox", "Drums - Loops/Loops - Full/Cymatics - Boombox Full Drum Loop 12 - 140 BPM.wav"),
    ("04_piano_house",   "Instruments/Piano Loops/Cymatics - House Piano Loop 3 - 128 BPM F Min.wav"),
    ("05_strings_house", "Instruments/String Loops/Cymatics - House String Loop 3 - 128 BPM A# Min.wav"),
    ("06_strings_moon",  "Instruments/String Loops/Cymatics - Moonlight String Loop 3 - 110 BPM G# Min.wav"),
    ("07_guitar_chord",  "Instruments/Guitar Loops/Cymatics - Guitar Chord Loop 2 - 128 BPM Dmaj.wav"),
    ("08_brass_moon",    "Instruments/Brass Loops/Cymatics - Moonlight Brass Loop 2 - 100 BPM A Min.wav"),
]


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("FAIL:", " ".join(str(c) for c in cmd), file=sys.stderr)
        print(r.stderr[-800:], file=sys.stderr)
        return None
    return r.stdout.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(HERE, "listening_pack"))
    ap.add_argument("--ratio", default="2.0")
    ap.add_argument("--pitch", default="0")
    ap.add_argument("--corpus", default=PACK,
                    help="root of the loop corpus (or set CHRONOS_CORPUS)")
    ap.add_argument("--baseline-bin", default=None,
                    help="path to a pre-fix chronos_file for BEFORE renders")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    chronos = os.path.join(ROOT, "build", "chronos_file")
    elastique = os.path.join(ROOT, "build", "elastique_file")
    tag = f"t{args.ratio}"
    if args.pitch not in ("0", "0.0"):
        tag += f"_p{args.pitch}"

    for label, rel in LOOPS:
        src = os.path.join(args.corpus, rel)
        if not os.path.exists(src):
            print("missing:", src, file=sys.stderr)
            continue
        shutil.copyfile(src, os.path.join(args.out, f"{label}_{tag}_0_INPUT.wav"))

        if args.baseline_bin and os.path.exists(args.baseline_bin):
            run([args.baseline_bin, src, os.path.join(args.out, f"{label}_{tag}_1_BEFORE.wav"),
                 "--time", args.ratio, "--pitch", args.pitch, "--engine", "auto", "--quality", "best"])

        info = run([chronos, src, os.path.join(args.out, f"{label}_{tag}_2_CHRONOS.wav"),
                    "--time", args.ratio, "--pitch", args.pitch,
                    "--engine", "auto", "--quality", "best"])
        run([elastique, src, os.path.join(args.out, f"{label}_{tag}_3_ELASTIQUE.wav"),
             "--time", args.ratio, "--pitch", args.pitch])
        engine = info.split()[0].split("=")[1] if info and "engine=" in info else "?"
        print(f"{label:18s} {tag:12s} chronos engine = {engine}")

    print("\npack written to", args.out)
    print("Listen in filename order: 0_INPUT, 1_BEFORE, 2_CHRONOS, 3_ELASTIQUE.")


if __name__ == "__main__":
    main()
