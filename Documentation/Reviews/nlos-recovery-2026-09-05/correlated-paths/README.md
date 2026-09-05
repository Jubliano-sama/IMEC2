# Testing correlated-path discounts and RMSE-guided reflection search

The user's hypothesis is that nearby anchors viewing a target in similar
directions may share an obstruction, so their errors should not count as fully
independent evidence. The first implementations of that idea did **not** improve
the recovered NLOS solver consistently. They remain research code; the GUI's
scoring and reflection search were not changed in this experiment.

## What was tested

The baseline here is **the recovered NLOS solver**, not the older Neighbor
intervals solver. All variants retain its original initialization, distance
weight power 1, positive-bias loss, radio intervals, and three reflection sweeps.
Only measured ranges and neighbor evidence enter the solver; wall labels and
truth are used for generation, scoring, and post-run diagnosis.

The initial pilot used 33 cases and four methods (132 solves). It compared
ordinary reflection search, a clustered-path discount, skipping anchors below
0.15 m incident-range RMSE, and both changes together. A cluster requires a
direct measured separation of at most 3 m between the neighboring anchors and
an angular separation of at most 20 degrees as viewed from the target's initial
estimated position. At each endpoint, the discount increases between 0.15 and
0.5 m RMSE, down to a minimum weight of 0.25. An edge is discounted once, even
if both endpoints supply evidence. The discount applies only to the positive
measured-excess part of the cost, ramps in from three to six noise sigmas,
and preserves both ordinary small-noise fitting and the full >8 m bias penalty.
These values are experimental assumptions, not calibrated environmental facts.

Groups and RMSE gates are frozen before comparing reflected candidates. This
prevents a trial from reducing its own penalty by moving anchors into a cluster
or deliberately increasing its residual. It does not make a wrong starting
layout a trustworthy source of group membership or uncertainty.

The pilot found regressions from discounting and missed corrections from
skipping low-RMSE anchors. A second experiment added two guards: both grouped
links must already show positive measured excess beyond three sigmas, and the
neighboring anchors' mutual measurement must fit their estimated separation
within three sigmas. It also replaced skipping with **prioritization**: visit
high-RMSE anchors first but still try every anchor.

That second experiment ran all 141 cases from the recovery verification with
four methods, for 564 solves. This includes repeated noise realizations,
relabelled layouts, and matched sparse versions; these are not 141 independent
environments. It is an exploratory extension of the existing benchmark, not
held-out deployment qualification. The strengthened parameters were fixed before
this full run and were not tuned to the full-run results.

## Results of the guarded experiment

| Cases | Current NLOS solver | Guarded group discount | RMSE priority only | Discount + priority |
| --- | ---: | ---: | ---: | ---: |
| Original sketch samples: local target flips / 20 | 0 | 3 | 0 | 3 |
| Fresh sketch noise/bias samples: local target flips / 24 | 3 | 7 | 3 | 7 |
| Anchor-ID permutations: local target flips / 12 | 1 | 3 | 1 | 3 |
| Random walls: median whole-map RMS | 0.724 m | 0.795 m | 0.803 m | 0.805 m |
| Strict degree-four wall schedules: median whole-map RMS | 2.920 m | 3.363 m | 1.978 m | 1.978 m |

All four methods rejected the same disconnected case. Medians exclude that
case, and `summary.json` records errors separately. The guarded variants left
the clean, clean degree-four, and short-link NLOS groups effectively unchanged.
The exact indistinguishable reflection remains wrong. The strict degree-four
schedule is a stress schedule, not the current GUI's rigidity-aware planner.

Across the 140 cases where all methods returned positions, discounting improved
whole-map RMS by more than 10 cm in one case and worsened it by more than 10 cm
in fourteen. Prioritization improved nine and worsened three, but it used 12,537
local optimization calls versus 12,212 for the baseline. It did not simplify
the search or reduce its work overall. The combination improved ten and worsened
fifteen. Paired variants are correlated samples; these counts are descriptive,
not independent statistical trials.

## Why a discount can still choose the wrong side

For `drawing_replay/1004/1.6`, the guarded rule discounted six links. Every one
of those links was actually NLOS according to the generator. Nevertheless, it
discounted the G1-to-upper-cluster links more strongly than the target's links,
making a wrong reflected layout cheaper:

| Returned layout | Original NLOS score | Frozen discounted score |
| --- | ---: | ---: |
| Baseline, correct target side | 80.333 | 70.265 |
| Discounted solver, wrong target side | 124.166 | 68.772 |

This is a scoring failure, not simply a missed optimization candidate. Those
biased links still help constrain the rest of the layout. Correctly suspecting
an obstruction does not make it harmless to weaken all their information.
RMSE also reflects disagreement with the current estimated geometry; it does
not independently measure positional uncertainty or identify the faulty node.

## A better formulation to investigate next

Represent a group's measurements as geometric distances plus a shared positive
offset and a remaining per-link error. That lets the common component carry
less independent evidence while preserving differences between measurements.
For example, if two measured ranges share exactly +2 m bias, subtracting the
ranges cancels that common 2 m while retaining a useful geometric constraint.
Actual paths through the same obstacle can have different offsets, so the
shared component must be soft rather than enforcing equality.

This shared-offset model is a proposal, **not implemented or validated here**.
It needs a new comparison including genuinely correlated offsets, unequal
offsets through the same wall, nearby paths passing on opposite sides of a
wall edge, and independent bad measurements. The recovered generator already
shares obstruction status and endpoint shadow factors, but draws each link's
additional bias independently conditional on those factors. It does not test
every kind of common physical path error.

Spatially correlated NLOS offsets have been modeled in published work, for
example [Joint trajectory and ranging offset estimation for accurate tracking
in NLOS environments](https://researchers.mq.edu.au/en/publications/joint-trajectory-and-ranging-offset-estimation-for-accurate-track/).
That work uses motion-sensor and trajectory information; it supports investigating
correlation, not claiming that the same improvement follows from this static
anchor survey alone.

## Reproduction and validation

```sh
python -m tools.gateway_gui.experiments.correlated_nlos_probe --out /tmp/correlated-paths.jsonl
python -m unittest tools.gateway_gui.tests.test_correlated_nlos_probe
```

`pilot-source.py`, `pilot.jsonl`, and `pilot.meta.json` preserve the initial
experiment exactly. The guarded implementation is
`tools/gateway_gui/experiments/correlated_nlos_probe.py`; its results are in
`guarded-validation.jsonl`, with source hashes in the adjacent metadata and
`environment.json`. Frozen generated inputs, including evaluation-only truth
and labels, are in `inputs.jsonl`. `summary.json` and `paired-changes.json`
contain aggregate results. No failed case was removed from these records.

Three boundary tests verify that the guarded rule needs both excess and observed
proximity, that weights stay fixed across trial layouts, and that discounts
preserve small residuals, positive model-minus-measurement errors, and bias-cap
growth. The full GUI suite passes 276 tests; compilation and mypy over 31 source
files also pass. These checks validate the implementation of the experiment;
its benchmark results do not justify enabling it in the GUI.
