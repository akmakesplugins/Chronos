# QA harness

Objective measurement for the engine. Synthetic signals hide the defects that only
real music exposes — every quality claim in [../docs/quality.md](../docs/quality.md)
came from running these scripts on a corpus of commercial loops.

The loop corpus itself is not distributed (licensed sample content). Point the
scripts at your own material; the paths at the top of `run_corpus.py` and
`make_listening_pack.py` are the only things to change.

## Setup

```bash
python3 -m venv .venv
.venv/bin/pip install numpy scipy soundfile matplotlib
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

`chronos_file` (built as an example target) is what renders the audio. If you also
have the élastique SDK, build an equivalent `elastique_file` CLI to get reference
renders — the corpus runner will use them if present, and simply skip the
comparison columns if not.

## The scripts

| Script | Does |
|---|---|
| `analyze.py` | Scores one render against its input and writes a stacked spectrogram (full view + onset zoom). The core measurement. |
| `preecho.py` | Per-onset attack quality: pre-echo level and HF rise time. The whole-file metrics cannot see these. |
| `run_corpus.py` | Sweeps a loop corpus at several stretch/pitch cases, scores everything, writes a scoreboard. |
| `compare_tags.py` | Diffs two corpus runs, so a change's effect on **every** metric is visible — including the ones it was not meant to touch. That is where regressions hide. |
| `make_listening_pack.py` | Renders input / before / after / reference sets for ear checks. |

## Typical loop

```bash
# once, before changing anything
.venv/bin/python qa/run_corpus.py --tag baseline

# ...change the engine, rebuild...

.venv/bin/python qa/run_corpus.py --tag my-change --skip-elastique
.venv/bin/python qa/compare_tags.py baseline my-change
```

## Reading the output

`compare_tags.py` prints, per metric, each run's win count against the reference
**and** its mean absolute deviation, with the reference's own mean in the last
column.

Read the means, not the win counts, when both engines sit far below the reference's
error: at that point wins are a coin flip on noise. This is not hypothetical — a
real fix once showed win counts falling 36→15 while the actual error *improved*
0.019→0.009, because both engines had reached the measurement floor.

Two more cautions:

- **`warble_cents` on percussive material is unreliable.** It tracks the strongest
  partial per band and conflates real frequency modulation with the tracker hopping
  between partials. Differences under ~5 cents are noise. Trust it on sustained
  tonal material.
- **Whole-file metrics cannot see pre-echo.** Always run `preecho.py` when touching
  anything transient-related.
