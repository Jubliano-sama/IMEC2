# Recovered NLOS solver: verification, 2026-09-05

The recovered approach substantially reduces the sketch's local reflection,
but it does not solve every NLOS layout. It is available in the GUI as
**NLOS one-sided intervals**. **Neighbor intervals** remains the default because
the new solver regressed on sparse surveys. All results here are synthetic;
no saved real survey was available and no hardware was flashed.

## Recovery and implementation

Claude's worktree was `t3code-14f7b03f`, branch `t3code/fix-nlos-anchor-flips`,
at base commit `107e72d866f5d32443c04191220f20d56211fc77`. It contained an
uncommitted design and implementation plan, but no installed solver. Its runnable
prototype and completed result files survived in `/tmp/nlos_probe`.
`claude-original.tar.gz` preserves those files and both documents, with all
18 original file hashes checked against `recovery-manifest.json`.
The source worktree was left untouched.

The implementation uses the measured ranges, their existing uncertainty, and
neighbor evidence. It receives no truth coordinates, wall locations, NLOS
labels, CIR, or signal-strength data. It combines three mechanisms:

1. Model distances longer than measured ranges keep the normal squared error.
   For shorter model distances, the cost saturates at 16 normalized units,
   allowing positive NLOS excess without pulling the geometry indefinitely.
   Beyond an assumed 8 m excess, the penalty grows again. Small measurement
   noise retains either sign; the positive-bias prior is about propagation,
   not a claim that every measurement error is positive.
2. Distance power 1 weights the cost by `median(range) / max(range, 0.3 m)`.
   The power is configurable from 0 to 4. Weighting leaves the noise sigma
   unchanged, so it does not broaden the loss's transition to saturation.
3. After local optimization, each unlocked anchor is reflected across lines
   through pairs of its four nearest measured neighbors, then optimized again.
   Only objective improvements are accepted, for at most three sweeps.

The saved implementation plan changed the tested seed search. Following that
plan produced two flips in the 20 replay cases. Restoring the runnable
prototype's actual Auto search removed both: a spring seed with 8 starts and
3 basin hops, a graph-MDS seed, and three perturbations of each using RNG 7 and
`0.08 * median(range)`, with 300 local evaluations. Explicit seed choices remain
available, but the results below use Auto. This is why copying the plan alone
would not have reproduced the claimed result.

The port uses the GUI's hard coordinate locks and existing radio constraints.
Possible NLOS links and loss of locally constraining near-fit ranges appear
under **Fit details**, in both geometry views. These are fit diagnostics, not
wall detections or confidence guarantees. All raw measured residuals remain
available, including those the objective relaxes.

## Results

The baseline is the existing Neighbor intervals solver with Auto seeds and
distance power 0. The candidate uses Auto and power 1. Both see identical input
data, with no truth supplied as a seed. The final verification contains 141
cases, each solved by both methods. Relabelled and capped cases are paired
variants, so this is not 141 independent physical layouts.

For the sketch, the flip metric tests which side of the nearby support line
the target occupies, ignoring a harmless global reflection of the whole map.

| Sketch cases | Cases | Baseline flips | Candidate flips | Median whole-map RMS, baseline / candidate |
| --- | ---: | ---: | ---: | ---: |
| Replayed original noise and bias samples | 20 | 18 | 0 | 1.421 / 0.234 m |
| Fresh noise and bias samples, same sketch geometry | 24 | 22 | 3 | 1.401 / 0.223 m |
| Anchor-ID permutations of 12 fresh cases | 12 | 11 | 1 | 1.401 / 0.218 m |

The replay covers ten samples each at 1.6–3 m and 3–5 m bias. The original
prototype also reproduced 0/20 when run directly. Thus the original sketch
claim holds, while the fresh samples show it is not a universal zero-flip result.

For arbitrary layouts, the recovered metric is **a LOS-group leave-one-out
position error greater than 1 m**, not a geometric mirror classifier. Each LOS
anchor is evaluated after aligning the other anchors in its group. That metric
can miss displacement or rotation of an entire group, so whole-map RMS and
worst-anchor errors are recorded separately. Alignments allow rigid reflection
but never rescale distances.

| Additional cases | Cases | Baseline group-error cases | Candidate group-error cases | Median whole-map RMS, baseline / candidate |
| --- | ---: | ---: | ---: | ---: |
| Fresh random wall geometries and variable radio reach | 24 | 3 | 1 | 0.700 / 0.724 m |
| Same wall cases, strict degree-four ranging cap | 24 | 19 + 1 solve error | 22 + 1 solve error | 2.132 / 2.920 m |
| Clean, all pairs measured | 12 | 0 | 0 | 0.023 / 0.026 m |
| Clean, strict degree-four ranging cap | 12 | 1 | 3 | 0.050 / 0.054 m |
| Four shortest links receive 0.7–2 m positive bias | 12 | 5 | 1 | 0.397 / 0.028 m |

Both solvers reject the same disconnected wall case; medians use successful
solves only. Every generated case is retained. The strict cap is a randomized
stress schedule, not a replay of the firmware/GUI's rigidity-aware planner.
Nineteen of the 24 capped wall input graphs have full local rigidity rank, as
do eleven of the twelve capped clean graphs. Local rigidity does not guarantee
global uniqueness, but sparse failures cannot all be dismissed as disconnected
or locally unconstrained input graphs.

In the uncapped fresh wall cases, the candidate improves median within-group
RMS from 0.175 to 0.035 m, yet whole-map median RMS slightly worsens and the
worst anchor error increases from 2.97 to 8.43 m. A coherent group can still be
placed incorrectly. Prefer richer merged data from **Survey All Neighbors**
when trying the new solver, and compare layouts against independently known
anchor positions.

Rescoring Claude's saved 144-case uncapped random-wall family also corrects its
zero-failure summary: the candidate has **3/144** cases above the group's 1 m
threshold, versus **12/144** for Neighbor intervals. Median within-group RMS
improves from 0.136 to 0.038 m; whole-map median RMS improves from 0.804 to
0.664 m. Those are different measurements. The original family generator
filters for connected/minimum-degree-three cases, and its cap-repair routine
can exceed the nominal cap. The new independent harness does neither.

Finally, the deliberately indistinguishable eight-anchor counterexample still
returns the wrong target, displaced by 6 m, with effectively zero range
residual and no fit warning. Its biased true position and unbiased reflected
position produce identical inputs. The near-fit rank diagnostic cannot detect
that discrete ambiguity. This approach does not infer walls or establish that
the real brown/blue deployment is now resolved.

## Reproduce and inspect

Run from the repository root using a Python environment with the GUI
requirements (the verification environment was
`/home/tommie/Projects/IMEC2/.venv/bin/python`):

```sh
python -m tools.gateway_gui.experiments.verify_nlos_recovery --out /tmp/nlos-verification.jsonl --workers 6
python -m tools.gateway_gui.experiments.rescore_nlos_archive Documentation/Reviews/nlos-recovery-2026-09-05/claude-original.tar.gz --out /tmp/nlos-archive-rescore.json
python -m unittest discover -s tools/gateway_gui/tests -v
```

The verifier accepts `--groups drawing_replay,drawing_unseen,drawing_relabelled`
for the sketch subset. JSONL rows preserve inputs, truth for scoring, returned
positions, solver errors, and separate metrics; source hashes accompany new
runs. Runtime measurements include concurrent worker contention.

Final checks passed: all 273 GUI unit/integration tests, including live Tk
embedded/fullscreen controls; mypy over 30 source files; Python compilation;
and `git diff --check`. These are host-side checks, not hardware qualification.

| File | Purpose |
| --- | --- |
| `claude-original.tar.gz`, `recovery-manifest.json` | Exact recovered source, documents, and saved results |
| `drawing-replay.csv` | Direct original-prototype replay against the existing solver |
| `archive-rescore.json` | Audit of saved original results, including the three candidate failure cases |
| `restored-search-validation.jsonl` | Final production search: 56 sketch and relabelled cases, 112 solves |
| `restored-search-general.jsonl` | Final production search: 85 additional cases, 170 solves |
| `restored-search-general.meta.json` | Source hashes and runtime environment for that general run |
| `validation-summary.json` | Aggregate final results from those two JSONL files |
| `plan-seed-port.py`, `independent-validation.jsonl` | Intermediate port with the plan's seed policy; superseded by the restored search |

Later changes bound and wrap diagnostic text and expose it through the GUI;
they do not alter the benchmarked objective or search. The intermediate run's
short-link-bias generator used set iteration to assign offsets; the final
generator sorts those keys for reproducibility, so compare methods within a
run rather than treating those intermediate offsets as identical final inputs.

## GUI usage

Choose **NLOS one-sided intervals**, leave **Seed** on Auto, and start with
**Distance weight power** 1. The GUI remembers the power separately for each
algorithm during the session. Coordinate locks remain exact. Inspect **Fit
details** for relaxed links and weak support. Ordinary **Refine measured
distances only** still uses a symmetric loss and can undo the NLOS correction;
use **Solve / re-solve** with the NLOS algorithm selected to retain its loss.

This recovery adds no firmware changes, CIR collection, or ML model. It is a
tested solver option with demonstrated limits, awaiting validation on real
survey data.

A follow-up tested the user's suggestion to discount correlated paths and
guide reflections using per-anchor RMSE. The [correlated-path experiment](correlated-paths/README.md)
found regressions, so those changes remain outside the GUI solver.

The subsequent [autonomous shared-offset experiments](shared-offset/README.md)
use the user's longer reach observations (LOS up to 20 m, NLOS around 15 m).
They found no reliable new accuracy improvement. A separately confirmed
speed-only change uses exact derivatives on richer graphs while preserving
the numerical calculation for degree-four-sized inputs.
