# Anchor geometry NLOS investigation

The drawing describes a local reflection of one node across its nearby anchors.
Distance weighting can reduce the influence of distant biased links, but cannot
guarantee the right side when the reliable links do not distinguish the sides.
No hardware capture was available. The examples below are constructed diagnostic
cases, not estimates of real-world failure rates.

Follow-up: Claude's prototype was recovered and integrated as the selectable
**NLOS one-sided intervals** solver. It improves sketch cases substantially but
regresses on sparse surveys; the existing default remains. See the
[recovery verification](nlos-recovery-2026-09-05/README.md) for paired results and
limits. The exact ambiguity demonstrated below remains unresolved.

## Current implementation

- `survey_runtime.py` correctly separates measured ranges from radio neighbors.
  Hearing in either direction supplies positive evidence; negative evidence
  requires both reports and excludes measured or positively observed pairs.
  An omitted degree-capped measurement does not itself become a negative edge.
- Both neighbor-interval and visibility polishing use symmetric squared range
  residuals. Live median survey ranges receive a fixed 0.05 m sigma; a stable
  wall-induced bias is not represented by that uncertainty.
- Negative radio evidence still incurs a penalty for placement closer than
  the configured radio minimum. Two nearby nodes separated by an obstructing
  wall can violate this assumption. The bitmap fixes measurement selection
  semantics; it does not establish a reliable minimum geometric separation.
- An automatically increased upper radio bound must not increase that lower
  bound. A long observed NLOS range also does not calibrate true radio coverage.

## Reproduction and results

Run from this checkout with the GUI Python environment:

```sh
python -m tools.gateway_gui.geometry_nlos_probe > /tmp/nlos-full.json
python -m tools.gateway_gui.geometry_nlos_probe --degree-cap 4 > /tmp/nlos-capped.json
```

The probe uses eight anchors: three nearby LOS supports, four supports behind
the illustrative wall, and the target. Only the four target-to-wall-side ranges
receive positive offsets (2.568–4.465 m). In the exact counterexample those
offsets deliberately equal the extra distance to the reflected target position.
The three LOS supports lie on the reflection axis. All other ranges are exact.
Thus the brown position with NLOS bias and the blue position with LOS ranges
produce identical measurements and neighbor evidence. The wrong position has
zero residual under every positive distance weighting.

The second case moves the middle LOS support 0.15 m off the axis. The degree-four
variant selects a fixed 16-edge schedule, retaining all 28 positive bitmap edges.
The two solvers receive automatic seeds, not truth coordinates. Evaluation aligns
only the other seven anchors, allowing rotation/reflection but no scale, so a
global coordinate-frame reflection is not counted as the local flip.

48 runs cover two solvers, powers 0/1/2/4, three cases, and two measurement graphs.
Both solvers produced the same results to the displayed precision:

| Case | Ranging graph | Target error, power 0 | Target error, power 4 | Unweighted range RMSE, power 0 |
| --- | --- | ---: | ---: | ---: |
| LOS control | Full | <0.001 m | <0.001 m | <0.001 m |
| LOS control | Degree 4 | <0.001 m | <0.001 m | <0.001 m |
| Exact reflection | Full | 6.000 m | 6.000 m | <0.001 m |
| Exact reflection | Degree 4 | 6.000 m | 6.000 m | <0.001 m |
| Nearly collinear supports | Full | 6.098 m | 6.097 m | 0.033 m |
| Nearly collinear supports | Degree 4 | 6.130 m | 6.104 m | 0.018 m |

The exact reflected cases produced no existing solver warnings. This demonstrates
why small residuals cannot certify position accuracy. These cases do not test
obstacle-dependent connectivity or establish general sparse-graph robustness.

## Experiment controls implemented

`Distance weight power` is shared by normal and fullscreen views and applies to
solve/re-solve, solving from a dragged seed, and measured-distance refinement.
For the symmetric solvers, power 0 preserves existing inputs. For power p, a range's original inverse-variance
weight is multiplied by `(shortest enabled range / range)**p`. At twice the
distance, p=1 gives half the weight and p=2 a quarter. The nearest enabled range
retains its weight. Existing uncertainty differences remain, and raw survey
records and radio constraints are unchanged. The result label records nonzero p.
This deliberately weakens distant range evidence relative to radio penalties too;
it cannot compensate for an incorrect radio-minimum assumption.
The recovered NLOS solver instead weights its one-sided cost relative to the
median range, retaining each range's original sigma so weighting does not move
the saturation threshold. It starts at power 1, remembered separately from
the symmetric solvers' default power 0.

New usable current-pass distances beyond `Neighbor max (m)` raise it to the
observed distance plus 1 m before automatic solving. Redraws and retained ranges
do not override a subsequent manual decrease. New or changed measurements can
raise it again. `Radio min (m)` remains independently adjustable.

Anchor positions can now be locked in the embedded and fullscreen geometry
views. Locks remove those coordinates from every solver's optimization variables
and bypass canonical output rotation. Seed layouts are rigidly aligned to the
locked frame before optimization, allowing a global reflection. Refinement
respects the same constraints. Fixed-to-fixed ranges still contribute to errors;
locking every node evaluates the selected positions without optimizing them.
Locks follow explicit whole-layout transforms, survive survey passes in this GUI
session, and can be individually unlocked or cleared together. Lock and frame
edits pause during a solve so a worker cannot return into a different frame.
Locking only collinear supports does not resolve the counterexample's ambiguity.

## Next algorithm experiment

The upstream harness at commit `01c3edb470bcd868403e04a6cded754360decdf0`
generates every link within a fixed 8 m radius and adds 3 cm Gaussian noise plus
independent uniform 0–20 cm positive offsets on one third of links. Its office
shapes describe anchor placement; that measurement generator does not intersect
paths with obstacles. See [the measurement generator](https://github.com/Jubliano-sama/AnchorGeometrySolver/blob/01c3edb470bcd868403e04a6cded754360decdf0/work/anchor_solver_p95_ml_cases.py#L147-L183)
and [noise parameters](https://github.com/Jubliano-sama/AnchorGeometrySolver/blob/01c3edb470bcd868403e04a6cded754360decdf0/work/anchor_solver_ml_distance_completion.py#L41-L51).

Extend its case context with separate discovered edges and selected measurements.
Generate obstacle crossings before visibility/ranging selection, with correlated
positive metre-scale biases, variable reach, asymmetric/lost discovery, and
degree-limited schedules. Keep generated NLOS labels and truth hidden from the
candidate solver; evaluate an oracle separately only as an upper reference.
Do not discard non-rigid cases as generation failures: measure whether they are
correctly identified as unresolved.

Compare this baseline and distance weighting with an asymmetric robust residual
model `observed = geometric distance + nonnegative bias + noise`, retaining
regularization on bias so arbitrary shortened layouts cannot fit for free.
Treat absence of radio contact as uncertain evidence rather than a guaranteed
minimum distance. Evaluate competing reflected solutions and weak geometry,
including the rate of confidently wrong answers, not merely measured RMSE.
Robust losses alone will still prefer the exact counterexample's perfect wrong
fit without additional evidence. A reliable non-collinear LOS connection,
trustworthy NLOS diagnostics, or a known placement/side constraint can provide
the missing evidence. The follow-up recovery adds a selectable algorithm,
while retaining the previous default and this exact counterexample.

## Inferring obstacles without CIR

The next useful target is a map of likely obstructions with visible uncertainty,
not a recovered architectural floor plan. No CIR collection or CIR-based
correction is part of this proposal. This is an experiment design, not an
implemented or validated wall predictor.

The current survey already provides median ranges, successful-sample counts,
separate neighbor reports, and coarse received signal levels. Signal levels are
quantized in 5 dB steps, with zero encoding unavailable data. The firmware stores
up to three neighbor-beacon samples, reports their median, and records only one
slot-ordered direction for signal strength. These are Channel 5 discovery
observations, so their attenuation must not be treated as a calibrated ranging
channel bias. They support a weak shared-obstruction hypothesis; they do not
provide a conversion from signal loss in dB to distance error in metres.

An initial experiment should use a small number of candidate wall segments:

1. Start with independently placed, locked anchors, ideally spread around the
   area. A lock asserts the operator's position; it does not certify an earlier
   solver estimate. Fit the unobstructed radio baseline using trusted links,
   allowing transmitter/receiver differences and noisy signal-strength bins.
2. Look for regions crossed by several links whose ranges are consistently
   longer than the known geometry, whose signals are unexpectedly weak, or
   whose repeated discovery attempts fail. Compare these with clean links
   crossing nearby regions. Keep an alternative per-anchor measurement fault
   explanation so one bad device does not generate an imaginary wall.
3. Score a few wall-segment hypotheses against these observations, penalizing
   additional segments and complexity. A crossing increases the probability
   of NLOS; it does not force a fixed offset on every crossing link. Use a
   positive-bias range model with a broad noise component. Treat missed radio
   observations probabilistically, with separate models for discovery and
   ranging channels. An edge omitted from the ranging schedule is unknown,
   not evidence of a barrier. Never interpret a missing signal-strength field
   as a weak received signal.
4. Infer candidate obstructions from links between trusted anchors, then test
   brown/blue position hypotheses using links held out from that inference.
   Repeat with whole anchors or batches held out, since links sharing an anchor
   have correlated errors. Do not create a wall from a suspect node's assumed
   position and then use that same wall as independent confirmation of it.
5. Show the family of similarly supported wall locations as an uncertain region.
   Areas without enough crossing paths remain unknown. An alternative coarse
   nonnegative attenuation grid with spatial regularization is appropriate if
   the observations reject a short-segment representation; a fine pixel grid
   would add unsupported detail to a sparse survey.

This direction is related to [Wilson and Patwari's radio tomographic
imaging](https://span.ece.utah.edu/uploads/RTI_version_3.pdf), which uses many
crossing RSS links and regularization to estimate attenuation. Their experiment
images changes caused by moving objects and removes static losses through
differencing. It does not establish that our static walls can be recovered from
one sparse survey with uncertain anchor coordinates and no empty-room baseline.
The static-wall extension above is an inference proposal requiring validation.

Without independent links through the obstructed area, the brown layout with
biased ranges and blue layout with unbiased ranges can remain equally compatible
with the observations. A wall prior alone cannot guarantee the correct choice.
The model should preserve that ambiguity rather than claim certainty from a low
range residual. Even with known node positions, many wall locations may intersect
exactly the same links, so wall thickness, material, and precise endpoints are
not identified by these measurements.

Start with the interpretable model before ML. Synthetic walls can create labels
for controlled comparison, but an ML model trained only on that generator can
learn its arbitrary bias/attenuation relationship. Compare generalization across
unseen layouts, materials, radio asymmetry, packet loss, fixed node faults, and
degree caps, then check held-out real environments before trusting it. Measure
local flip rate, position error, confidently wrong results, false wall detections,
and coverage of the reported uncertain regions. Also include clean environments
and deliberately indistinguishable reflection cases.

If later firmware data collection is justified, prioritize individual range
attempts (or compact min/median/max/spread summaries), per-link attempt counts,
and bidirectional signal levels on the ranging channel. Repetition reveals
instability but cannot remove a stable NLOS offset. One deliberately placed
temporary anchor, or a moved calibration node at known positions, can provide
the missing crossing paths and may be more informative than many repetitions
of the same ambiguous links. No firmware or radio schedule was changed here.
