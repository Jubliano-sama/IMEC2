# Superseded Strategy

This document described the earlier BLE-gated wake and discovery strategy. It is
kept only as a historical pointer so old links do not lead readers into an
obsolete implementation plan.

Use `Documentation/From Click to Ranging, Developing a Robust UWB Gated Wake up Strategy.md`
as the source of truth for the current firmware architecture. The current
design uses UWB for wake claims, discovery, arbitration, DS-TWR ranging, and
mesh transport. Operational BLE wake and mesh paths have been retired.
