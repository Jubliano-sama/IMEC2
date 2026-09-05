# Holdout plan, frozen before running the holdout

The development suite contains 87 cases: 33 cases from earlier experiments and
54 new cases. The analytic derivative experiment additionally covers all 141
earlier recovery cases. These are development results, not new confirmation.

Evaluate the following fixed methods on 144 generated holdout cases using
seeds 81000–81015, which have not been solved or inspected during development:

- Current NLOS solver, Auto and distance weight power 1.
- Shared positive-offset mixture, 0.3 m allowed per-link deviation, strength 0.5.
- The same shared model at strength 1.0, to measure its accuracy/regression tradeoff.
- Triangle-bound preprocessing with three-sigma path slack, followed by the current solver.
- The current objective and search using exact analytic derivatives.

Shared-model groups require observed mutual proximity within 3 m, mutual fit
within three sigmas and at most 20 degrees of angular separation in the
incumbent layout. Groups are frozen, disjoint and contain two to four edges.
The competing common-offset model pays one NLOS activation cost plus a squared
penalty for differing offsets; it can never discount a group with a too-short
measurement. Positive model residuals, small-noise fitting and >8 m excess
penalties are retained. These are assumptions, not environmental calibration.

There are sixteen cases each of shared offsets, unequal offsets, a wall gap,
clean measurements, random shared-offset layouts, and random unequal-offset
layouts. Shared, clean, and random-shared cases also have matching degree-four
versions selected by the current GUI planner from randomized discovery slots.
Anchor positions and per-anchor radio reach vary. Paired sparse versions and
different bias conditions sharing a seed are not independent environments.

Do not change configurations after examining holdout outcomes. Retain every
generated case, including failed or incomplete solves. Compare local target
flips only where the geometry defines that metric, and always retain whole-map
RMS, worst-anchor error and solver failures. Within-group fit alone is inadequate.

Promotion requires more than an improved aggregate median: inspect clean
controls, sparse cases, new flips, and newly large position errors. Shared
models already have development regressions, so a favorable small subset does
not qualify them for the default GUI solver. Exact derivatives must pass
numerical derivative checks and retain acceptable positioning behavior before
replacing the numerical implementation. If the evidence remains mixed, preserve
the experiment and report the limitation rather than install a speculative fix.

Before inspecting any holdout results, add an extra-search control: run the
same post-baseline refinement and reflection passes with shared-model strength
zero. This separates the effect of the new score from simply spending more
search effort. It runs in a separate file on the same unchanged cases.

## Updated deployment assumption supplied by the user

The user reports LOS reach up to 20 m and NLOS reach around 15 m and asks us
to emphasize less sparse surveys. Preserve the original runs as the legacy
reach profile. The new expanded profile uses LOS endpoint reaches of 17–20 m,
NLOS endpoint reaches of 12–15 m, and 5% ranging omissions while retaining their
positive neighbor evidence. These lower endpoints and omission rate are test
assumptions; only the approximate upper reaches were provided by the user.
Random layouts span up to 24 m in width, so long reach is exercised. The solver
uses a 20 m neighbor upper control, raised to maximum observed range plus 1 m
where needed. The existing 7 m radio minimum remains in these comparisons;
this does not establish that radio absence is a universal geometric lower bound.

Expanded development uses seeds 151000–151005. Expanded holdout uses previously
unused seeds 251000–251015. Run the same fixed baseline, shared_03,
shared_03_full, metric, analytic, and extra_search methods, with no tuning after
the expanded holdout is inspected. There are 96 primary merged-survey cases
and twelve secondary degree-four planner controls. Report them separately so
sparse controls do not dominate the deployment comparison.

## Separate confirmation for a proposed speed-only change

Development found two sparse numerical-path regressions from analytic
derivatives, including a clean degree-four case with effectively identical
objective scores at different positions. Preserve the old numerical search
when selected measured edges are no more than twice the anchor count; use
exact derivatives only above that threshold. This is a compatibility boundary
for degree-four plans, not a claim that larger graphs guarantee accuracy.

Because this conditional implementation was proposed during the expanded
holdout run, do not describe that existing run as an untouched confirmation
of the conditional choice. Freeze it now and run an additional confirmation
with unused seeds 351000–351007: 48 primary merged-survey cases plus twelve
degree-four controls. Compare baseline against dense_analytic with the same
objective, seed search, bounds and reflection trials. No shared-offset or
triangle-bound change is included in this candidate. Do not tune after reading
these results. Require no added flips or >10 cm whole-map RMS regressions in
this confirmation, plus derivative, lock and GUI integration checks before
promotion. This remains synthetic validation, not hardware qualification.
