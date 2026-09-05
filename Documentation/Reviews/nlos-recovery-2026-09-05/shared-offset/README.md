# Autonomous NLOS experiments: outcome

Stopped at the user's request once the tested accuracy changes showed no
reliable improvement. This does not prove that all possible accuracy approaches
are exhausted. Shared offsets, triangle-bound preprocessing and extra search
remain research code. The only promoted change is faster optimization for
denser surveys using exact derivatives of the existing objective.

## Deployment assumptions and methodology

The user reports LOS reach up to 20 m and NLOS reach around 15 m and requested
less emphasis on sparse surveys. The final expanded profile samples endpoint
LOS reach at 17–20 m and NLOS reach at 12–15 m. Those lower endpoints and a 5%
ranging-omission rate are experimental assumptions, not measured deployment
statistics. An omitted range retains positive neighbor evidence. Random layouts
span up to 24 m in width. The final primary test has median measured degree
6.73; its longest generated geometric links are 18.90 m LOS and 13.79 m NLOS.
Thus the sampled cases exercise longer links but do not qualify actual RF reach.

Every method sees the same ranges and neighbor graph, without truth, wall
locations or generated NLOS labels. The radio maximum is 20 m, raised to maximum
observed range plus 1 m if needed. The existing radio minimum is 7 m; the tests
do not establish that absent contact universally proves geometric separation.
Twelve degree-four controls use the actual GUI planner and remain separate from
the 96 primary merged-survey cases. Paired conditions and planner variants are
not independent environments.

Development, holdout and a separate speed-change confirmation use disjoint
seeds, documented in [the frozen plans](holdout-plan.md). Earlier legacy-reach
runs remain saved separately. No parameter was tuned after inspecting its
respective confirmation set.

## Accuracy results on the final expanded holdout

All 96 primary cases returned positions. A geometric target-flip metric applies
to the 64 sketch-based cases; the 32 arbitrary random layouts are evaluated by
whole-map errors instead. Whole-map alignment allows rotation and reflection
but no scaling. Counts below compare the same cases against the current NLOS
solver, not against the older Neighbor intervals solver.

| Method | Target flips / 64 | Flips fixed / newly introduced | Whole-map RMS improved / worsened by >10 cm, out of 96 | Median whole-map RMS |
| --- | ---: | ---: | ---: | ---: |
| Current NLOS solver | 19 | — | — | 0.465 m |
| Shared-offset model, strength 0.5 | 19 | 3 / 3 | 4 / 5 | 0.465 m |
| Shared-offset model, strength 1.0 | 17 | 7 / 5 | 10 / 19 | 0.514 m |
| Triangle-bound preprocessing | 22 | 0 / 3 | 1 / 4 | 0.496 m |
| Additional refinement/reflection passes | 19 | 0 / 0 | 0 / 0 | 0.465 m |

The shared model groups two to four links using observed mutual proximity,
similar direction and consistency of the neighbors' mutual ranges. Groups are
frozen before trying alternative layouts. A group can use independent NLOS
penalties or one common positive offset plus squared penalties for differences
between its implied offsets. It preserves small-noise fitting, positive
model-minus-measurement penalties, and the original >8 m excess penalty.
Even with those safeguards, a stronger shared prior traded fixes for new flips
and worsened the full map more often than it improved it.

Triangle bounds cap a direct range only when a measured path through other
anchors is shorter, with three-sigma noise slack. No missing range is filled
and original records are retained. Although such a path supplies useful
geometric evidence under positive-bias assumptions, this preprocessing still
changed the optimizer's preferred result unfavorably in several cases.

An extra-search control separates the shared model from merely spending more
optimization effort. It supplied no material accuracy improvement here. No
new wall predictor, CIR processing, firmware change or trained model was added.
The original exactly indistinguishable reflection remains unresolved.

## Promoted speed improvement

The existing solver estimated derivatives by repeatedly perturbing coordinates
and recalculating the score. The new code computes those derivatives directly,
including both branches of the one-sided loss and the radio interval hinges.
It retains the same objective, starting layouts and reflection trials.

Using exact derivatives everywhere changed a few sparse outcomes, including
a clean planner case with effectively equal scores at different positions.
The installed implementation therefore preserves numerical derivatives when
the selected measured graph has at most twice as many edges as anchors. A
degree-four plan cannot exceed that boundary. Above it, exact derivatives
accelerate richer merged surveys. This is a compatibility choice, not a
guarantee that dense graphs are unambiguous.

The conditional implementation was frozen and tested on an additional 60 fresh
cases: 48 primary merged surveys and twelve degree-four controls. All returned
positions. The primary cases retained 8 flips among the 32 cases defining the
target-flip metric, with no newly introduced flips. The maximum change in
whole-map RMS was 0.000000151 m. Median speedup was 2.43 times under the paired
worker benchmark. Sparse controls retained exactly the original positions and
runtime path. Runtime figures include concurrent worker contention.

After integration, all 60 cases were solved again through the actual GUI
dispatch function. Every returned coordinate exactly matched the confirmed
conditional implementation. All 282 GUI tests pass, including numerical checks
of derivatives, radio hinges, locked-coordinate derivatives, dense hard locks,
solver selection boundaries, and embedded/fullscreen controls. Compilation,
mypy over 34 source files, and whitespace checks passed before the final
documentation update. These are host-side checks, not hardware qualification.

## Evidence and reproduction

The research scripts are under `tools/gateway_gui/experiments/`. Numerical
baseline subclasses explicitly retain the old finite-difference calculation,
so adopting faster derivatives in production does not silently alter the
experimental controls. Source snapshots and SHA-256 metadata preserve the
implementations used for earlier runs.

```sh
python -m tools.gateway_gui.experiments.shared_bias_probe --reach-profile expanded --split holdout --methods baseline,shared_03,shared_03_full,metric,analytic,extra_search --out /tmp/nlos-expanded.jsonl
python -m tools.gateway_gui.experiments.shared_bias_probe --reach-profile expanded --split confirmation --methods baseline,dense_analytic --out /tmp/nlos-confirmation.jsonl
python -m unittest discover -s tools/gateway_gui/tests
```

| Artifact | Meaning |
| --- | --- |
| `development.jsonl` | 87 development cases, five shared-model/baseline variants |
| `analytic-development.jsonl` | 141 earlier recovery cases, numerical versus exact derivatives |
| `metric-development.jsonl` | Triangle-bound comparison on development cases |
| `holdout.jsonl`, `holdout-control.jsonl` | Legacy-reach holdout and separate extra-search control |
| `expanded-development.jsonl` | 48 development cases using the longer reach assumptions |
| `expanded-holdout.jsonl` | Final 96 primary cases plus twelve sparse controls, six methods |
| `dense-analytic-confirmation.jsonl` | Separate 60-case confirmation of the exact promoted conditional rule |
| `production-confirmation.jsonl` | Actual GUI-dispatch results matching all 60 confirmed layouts |
| `final-comparisons.json` | Paired results, explicit flip-metric denominators, and separate sparse results |
| Adjacent `*.inputs.jsonl`, `*.meta.json`, source snapshots | Generated inputs, evaluation-only truth, fixed settings and source hashes |

No failed case was removed from saved records. New experiments stopped after
the user asked to stop if reliable accuracy improvement was exhausted.
