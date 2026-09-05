# Delivery and route cleanup qualification — 2026-09-05

The final source passed both three-anchor prototype topologies. Reports now use one bounded four-packet C5 owner, including idle-relay transit singletons. Partial ACKs retain exact remaining members, completion failures retain terminal ACK proof, and new queue arrivals cannot shorten the retained packet's backoff. Admission includes bank identities under the same lock as queue-to-bank transfer. ACK reception queues unrelated traffic before yielding and bounds its standard-wake probe by the original ACK deadline. Route cost is derived; correlated depth/credit feedback immediately reselects through shared parent eligibility, and depth 255 remains unreachable after hold expiry.

| Final topology | Survey | Click completion | Reports / exact host receipts |
|---|---|---|---|
| F1F1D | 3 anchors, 3 pairs, 15/15 samples, no partial flags | 10/10 | 30/30 |
| F2F1D | 3 anchors, 2 visible pairs, 10/10 samples, no partial flags | 10/10 | 30/30 after additional drain capture |

Every final click produced one report from each anchor; every report contained four range samples. F2F1D had 25 reports and receipts at the end of the initial 100-second capture. The remaining five were observed in the subsequent passive drain capture. It therefore passes retained delivery, with a material latency limitation under ten clicks spaced five seconds apart. This is not evidence of low-latency or long-duration qualification.

The corrected F2F1D trace shows four-packet banks at C, B and A, terminal ACKs at C→B, B→A and A→gateway, and no report entering the legacy tracked owner. RX at B records source C / previous C, and RX at A records source C / previous B. BLE decode and exact host receipts establish gateway/host delivery; the gateway has no debug probe. Forced-hop RF-scope isolation is not a physical-distance or hidden-terminal proof.

The first cleanup version passed F1F1D but delivered only 9/30 F2F1D reports because the idle relay still bypassed the bank. That failed run is preserved in `f2f1d-clicks`; only `final-*` logs qualify the final source. `final-f2f1d-combined` concatenates the two consecutive captures and its summary deduplicates exact source/session/sequence identities. All captures are stopped. The bench is left with A direct, B forced-1, C forced-1, and the normal three-anchor clicker with RTT gesture injection.

Four focused native tests pass. The final mesh_integration run passed 165/165 and hardware_models passed 157/157. Added production-boundary regressions cover actual ACK depth updates, unreachable-parent hold expiry, singleton/partial/full-bank custody, unrelated ACK-window RX, PHY activity/deadline handling, click handoff after release, completion errors, semantic redrive, retry identity and all eighteen report/next-hop/core-activity forwarding combinations. These tests do not establish dense fifty-anchor or eight-hop physical operation.

All five builds succeeded. Unallocated RAM margins are direct anchor 6,336 bytes, forced anchors 6,400 bytes, clicker 33,328 bytes, and gateway 5,408 bytes. The three anchors and clicker were flashed using normal sector erase at 4 MHz. The gateway was built but remains on its earlier wire-compatible image because its debug probe is disconnected. `final-builds.json` records binary hashes; live FICR bytes and flash outputs are saved alongside the captures. The report translation unit decreased from 30,365 to 27,932 lines (2,433 removed net), plus the small explicit delivery-state header.

Remaining real-hop work is recorded in `Documentation/Reviews/real-hop-audit-2026-09-05.md`: the new local-solicit application/reply path is incomplete, mixed receiver scan intervals violate the claimed wake coverage at permitted settings, and dense hidden-terminal/large-system coverage is missing. The prototype still uses existing correlated route acquisition and does not fabricate direct gateway success.

This record describes qualification before the production-role deployment. Compact summaries and native test outputs are committed here; the full RTT and BLE captures remain in the local evidence folder.
