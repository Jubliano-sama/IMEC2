#!/usr/bin/env python3
"""Monte Carlo model for a compact, depth-banded enumeration response lane.

This is deliberately standalone design tooling.  It does not import or modify
the production enumeration implementation.  The PHY airtime calculation is a
literal translation of dwm3000_timing_airtime_rctu() for the production
standard-PHR Channel-5 profile.

The modeled protocol is:

* one shortest-path parent is selected per anchor for the operation;
* deepest hop-depth bands run first;
* a sender packs up to ten ``{anchor_id, hop}`` records per response;
* every unresolved bundle chooses a fresh random cell in every retry round;
* parents listen for the whole response part of a cell, then immediately ACK;
* a parent retains a decoded bundle even if its ACK collides;
* a band ends only after its fixed number of rounds;
* success requires every record at the gateway and every custody ACK complete.

RF reachability and interference both use orthogonal, one-grid-edge links.
Frames decode only when exactly one audible transmitter occupies the response
or ACK part of the cell.  Nodes cannot receive while transmitting, but the
response and ACK portions do not overlap inside a correctly sized cell.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import json
import math
import random
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Sequence


# Keep these values aligned with firmware/include/dwm3000_timing.h.
CH5_PREAMBLE_SYMBOLS = 4096
CH5_SFD_SYMBOLS = 16
PRF64_CHIPS_PER_SHR_SYMBOL = 508
PHR_BITS = 21
CHIPS_PER_BIT_SYMBOL_850K = 512
RCTU_PER_CHIP = 128
RCTU_PER_SECOND = 63_897_600_000
RS_DATA_BITS_PER_BLOCK = 330
RS_PARITY_BITS_PER_BLOCK = 48
FCS_BYTES = 2
STANDARD_FRAME_MAX_BYTES_WITHOUT_FCS = 125

RESPONSE_ENVELOPE_BYTES = 31
RECORD_BYTES = 9
RECORDS_PER_BUNDLE = 10
ACK_BYTES = 30
ACK_TURNAROUND_GUARD_US = 10_000
CELL_US = 25_000

Coord = tuple[int, int]


def ch5_airtime_us(frame_bytes_without_fcs: int) -> int:
    """Return production Channel-5 standard-PHR airtime, rounded up."""

    if not 1 <= frame_bytes_without_fcs <= STANDARD_FRAME_MAX_BYTES_WITHOUT_FCS:
        raise ValueError("standard-PHR frame length must be in [1, 125]")
    data_bits = (frame_bytes_without_fcs + FCS_BYTES) * 8
    rs_blocks = math.ceil(data_bits / RS_DATA_BITS_PER_BLOCK)
    coded_bits = data_bits + rs_blocks * RS_PARITY_BITS_PER_BLOCK
    total_chips = (
        (CH5_PREAMBLE_SYMBOLS + CH5_SFD_SYMBOLS)
        * PRF64_CHIPS_PER_SHR_SYMBOL
        + PHR_BITS * CHIPS_PER_BIT_SYMBOL_850K
        + coded_bits * CHIPS_PER_BIT_SYMBOL_850K
    )
    total_rctu = total_chips * RCTU_PER_CHIP
    return math.ceil(total_rctu * 1_000_000 / RCTU_PER_SECOND)


MAX_RESPONSE_BYTES = RESPONSE_ENVELOPE_BYTES + RECORDS_PER_BUNDLE * RECORD_BYTES
MAX_RESPONSE_AIRTIME_US = ch5_airtime_us(MAX_RESPONSE_BYTES)
ACK_AIRTIME_US = ch5_airtime_us(ACK_BYTES)
MIN_CELL_US = (
    MAX_RESPONSE_AIRTIME_US + ACK_TURNAROUND_GUARD_US + ACK_AIRTIME_US
)
CELL_MARGIN_US = CELL_US - MIN_CELL_US


@dataclass(frozen=True)
class SimConfig:
    slots_per_round: int
    rounds: int
    cell_us: int = CELL_US
    records_per_bundle: int = RECORDS_PER_BUNDLE

    def validate(self) -> None:
        if self.slots_per_round < 1:
            raise ValueError("slots_per_round must be positive")
        if self.rounds < 1:
            raise ValueError("rounds must be positive")
        if self.records_per_bundle < 1:
            raise ValueError("records_per_bundle must be positive")
        response_bytes = RESPONSE_ENVELOPE_BYTES + self.records_per_bundle * RECORD_BYTES
        if response_bytes > STANDARD_FRAME_MAX_BYTES_WITHOUT_FCS:
            raise ValueError("response bundle does not fit standard PHR")
        min_cell_us = (
            ch5_airtime_us(response_bytes)
            + ACK_TURNAROUND_GUARD_US
            + ACK_AIRTIME_US
        )
        if self.cell_us < min_cell_us:
            raise ValueError(
                f"cell_us={self.cell_us} truncates complete response/ACK airtime; "
                f"minimum is {min_cell_us}"
            )


@dataclass(frozen=True)
class Bundle:
    sender: Coord
    parent: Coord
    sequence: int
    records: tuple[Coord, ...]

    @property
    def key(self) -> tuple[Coord, int]:
        return self.sender, self.sequence


@dataclass
class TrialResult:
    success: bool
    data_complete: bool
    custody_complete: bool
    response_attempts: int
    response_collisions: int
    ack_collisions: int
    unacked_bundles: int
    missing_records: int
    bundle_count: int


@dataclass
class CampaignResult:
    max_hop: int
    anchor_count: int
    slots_per_round: int
    rounds: int
    cell_us: int
    interval_s: float
    trials: int
    successes: int
    success_rate: float
    data_complete_rate: float
    custody_complete_rate: float
    mean_response_attempts: float
    mean_response_collisions: float
    mean_ack_collisions: float
    worst_unacked_bundles: int
    worst_missing_records: int
    seed: int
    elapsed_s: float


def depth(node: Coord) -> int:
    return abs(node[0]) + abs(node[1])


def adjacent(a: Coord, b: Coord) -> bool:
    return abs(a[0] - b[0]) + abs(a[1] - b[1]) == 1


def nodes_at_depth(nodes: Iterable[Coord], band_depth: int) -> list[Coord]:
    return [node for node in nodes if depth(node) == band_depth]


def audible_bundles(receiver: Coord, attempts: Iterable[Bundle]) -> list[Bundle]:
    return [bundle for bundle in attempts if adjacent(bundle.sender, receiver)]


def audible_ack_parents(child: Coord, ack_parents: Iterable[Coord]) -> list[Coord]:
    return [parent for parent in ack_parents if adjacent(child, parent)]


def shell(hop: int) -> list[Coord]:
    if hop < 1:
        return []
    nodes: list[Coord] = []
    for x in range(-hop, hop + 1):
        y_abs = hop - abs(x)
        if y_abs == 0:
            nodes.append((x, 0))
        else:
            nodes.append((x, -y_abs))
            nodes.append((x, y_abs))
    return sorted(nodes)


def full_diamond(max_hop: int) -> list[Coord]:
    return [node for hop in range(1, max_hop + 1) for node in shell(hop)]


def diamond_count(max_hop: int) -> int:
    return 2 * max_hop * (max_hop + 1)


def make_topology(max_hop: int, anchor_count: int, rng: random.Random) -> list[Coord]:
    """Build full inner shells plus a random connected outer-shell subset."""

    if max_hop < 1:
        raise ValueError("max_hop must be positive")
    inner_count = diamond_count(max_hop - 1)
    full_count = diamond_count(max_hop)
    if not inner_count < anchor_count <= full_count:
        raise ValueError(
            f"max_hop={max_hop} requires {inner_count + 1}..{full_count} anchors"
        )
    nodes = full_diamond(max_hop - 1)
    outer = shell(max_hop)
    if anchor_count - inner_count < len(outer):
        outer = rng.sample(outer, anchor_count - inner_count)
    nodes.extend(outer)
    return sorted(nodes, key=lambda node: (depth(node), node[0], node[1]))


def choose_shortest_parents(
    nodes: Sequence[Coord], rng: random.Random
) -> dict[Coord, Coord]:
    node_set = set(nodes)
    node_set.add((0, 0))
    parents: dict[Coord, Coord] = {}
    for node in nodes:
        candidates = [
            other
            for other in node_set
            if depth(other) == depth(node) - 1 and adjacent(node, other)
        ]
        if not candidates:
            raise AssertionError(f"topology left {node} without a shortest parent")
        parents[node] = rng.choice(candidates)
    return parents


def _assign_distinct_slots(
    pending: Sequence[Bundle],
    slots_per_round: int,
    previous_slots: dict[tuple[Coord, int], int],
    rng: random.Random,
) -> list[tuple[int, Bundle]]:
    """Schedule at most one bundle per sender per cell, avoiding its last cell."""

    candidates = list(pending)
    rng.shuffle(candidates)
    candidates = candidates[:slots_per_round]
    chosen_slots: list[int] | None = None
    for _ in range(100):
        proposal = rng.sample(range(slots_per_round), len(candidates))
        if all(
            proposal[index] != previous_slots.get(bundle.key)
            for index, bundle in enumerate(candidates)
        ):
            chosen_slots = proposal
            break
    if chosen_slots is None:
        # This can only be reached in a saturated, awkward matching.  A tiny
        # backtracker preserves the fresh-cell rule instead of silently reusing
        # a previous cell because a greedy assignment painted itself in a corner.
        def find_matching(index: int, free: set[int], result: list[int]) -> bool:
            if index == len(candidates):
                return True
            disallowed = previous_slots.get(candidates[index].key)
            choices = [slot for slot in free if slot != disallowed]
            rng.shuffle(choices)
            for slot in choices:
                result.append(slot)
                if find_matching(index + 1, free - {slot}, result):
                    return True
                result.pop()
            return False

        chosen_slots = []
        if not find_matching(0, set(range(slots_per_round)), chosen_slots):
            raise AssertionError("fresh distinct cell assignment unexpectedly impossible")

    scheduled: list[tuple[int, Bundle]] = []
    for bundle, slot in zip(candidates, chosen_slots, strict=True):
        previous_slots[bundle.key] = slot
        scheduled.append((slot, bundle))
    return scheduled


def run_trial(
    max_hop: int,
    anchor_count: int,
    config: SimConfig,
    rng: random.Random,
) -> TrialResult:
    config.validate()
    nodes = make_topology(max_hop, anchor_count, rng)
    node_set = set(nodes)
    parents = choose_shortest_parents(nodes, rng)
    retained: dict[Coord, set[Coord]] = {node: {node} for node in nodes}
    retained[(0, 0)] = set()

    response_attempts = 0
    response_collisions = 0
    ack_collisions = 0
    total_unacked = 0
    total_bundles = 0

    for band_depth in range(max_hop, 0, -1):
        band_senders = nodes_at_depth(nodes, band_depth)
        bundles_by_sender: dict[Coord, list[Bundle]] = {}
        for sender in band_senders:
            ordered_records = sorted(retained[sender])
            chunks = [
                ordered_records[index:index + config.records_per_bundle]
                for index in range(0, len(ordered_records), config.records_per_bundle)
            ]
            bundles_by_sender[sender] = [
                Bundle(sender, parents[sender], sequence, tuple(chunk))
                for sequence, chunk in enumerate(chunks)
            ]
        total_bundles += sum(len(items) for items in bundles_by_sender.values())
        pending = {
            bundle.key: bundle
            for items in bundles_by_sender.values()
            for bundle in items
        }
        previous_slots: dict[tuple[Coord, int], int] = {}

        for _round in range(config.rounds):
            if not pending:
                break
            by_slot: dict[int, list[Bundle]] = {
                slot: [] for slot in range(config.slots_per_round)
            }
            for sender in band_senders:
                sender_pending = [
                    bundle for bundle in pending.values() if bundle.sender == sender
                ]
                for slot, bundle in _assign_distinct_slots(
                    sender_pending,
                    config.slots_per_round,
                    previous_slots,
                    rng,
                ):
                    by_slot[slot].append(bundle)

            for slot in range(config.slots_per_round):
                attempts = by_slot[slot]
                if not attempts:
                    continue
                response_attempts += len(attempts)

                decoded: list[Bundle] = []
                receivers = {(0, 0)} if band_depth == 1 else {
                    node for node in nodes_at_depth(nodes, band_depth - 1)
                }
                for receiver in receivers:
                    audible = audible_bundles(receiver, attempts)
                    if len(audible) == 1 and audible[0].parent == receiver:
                        bundle = audible[0]
                        decoded.append(bundle)
                        retained[receiver].update(bundle.records)

                decoded_keys = {bundle.key for bundle in decoded}
                response_collisions += sum(
                    1 for bundle in attempts if bundle.key not in decoded_keys
                )

                ack_transmitters = [bundle.parent for bundle in decoded]
                for bundle in decoded:
                    audible_acks = audible_ack_parents(
                        bundle.sender, ack_transmitters
                    )
                    if len(audible_acks) == 1 and audible_acks[0] == bundle.parent:
                        pending.pop(bundle.key, None)
                    else:
                        ack_collisions += 1

        total_unacked += len(pending)

    missing_records = len(node_set - retained[(0, 0)])
    data_complete = missing_records == 0
    custody_complete = total_unacked == 0
    return TrialResult(
        success=data_complete and custody_complete,
        data_complete=data_complete,
        custody_complete=custody_complete,
        response_attempts=response_attempts,
        response_collisions=response_collisions,
        ack_collisions=ack_collisions,
        unacked_bundles=total_unacked,
        missing_records=missing_records,
        bundle_count=total_bundles,
    )


def run_campaign(
    max_hop: int,
    anchor_count: int,
    config: SimConfig,
    trials: int,
    seed: int,
) -> CampaignResult:
    config.validate()
    rng = random.Random(seed)
    started = time.monotonic()
    successes = 0
    data_complete = 0
    custody_complete = 0
    response_attempts = 0
    response_collisions = 0
    ack_collisions = 0
    worst_unacked_bundles = 0
    worst_missing_records = 0
    for _ in range(trials):
        result = run_trial(max_hop, anchor_count, config, rng)
        successes += result.success
        data_complete += result.data_complete
        custody_complete += result.custody_complete
        response_attempts += result.response_attempts
        response_collisions += result.response_collisions
        ack_collisions += result.ack_collisions
        worst_unacked_bundles = max(worst_unacked_bundles, result.unacked_bundles)
        worst_missing_records = max(worst_missing_records, result.missing_records)
    elapsed_s = time.monotonic() - started
    return CampaignResult(
        max_hop=max_hop,
        anchor_count=anchor_count,
        slots_per_round=config.slots_per_round,
        rounds=config.rounds,
        cell_us=config.cell_us,
        interval_s=max_hop * config.slots_per_round * config.rounds
        * config.cell_us / 1_000_000,
        trials=trials,
        successes=successes,
        success_rate=successes / trials,
        data_complete_rate=data_complete / trials,
        custody_complete_rate=custody_complete / trials,
        mean_response_attempts=response_attempts / trials,
        mean_response_collisions=response_collisions / trials,
        mean_ack_collisions=ack_collisions / trials,
        worst_unacked_bundles=worst_unacked_bundles,
        worst_missing_records=worst_missing_records,
        seed=seed,
        elapsed_s=elapsed_s,
    )


def topology_matrix() -> list[tuple[int, int]]:
    return [
        (1, 4),
        (2, 12),
        (3, 24),
        (4, 40),
        *((5, count) for count in range(41, 51)),
    ]


def sweep(
    topologies: Iterable[tuple[int, int]],
    slots: Iterable[int],
    rounds: Iterable[int],
    trials: int,
    seed: int,
    jobs: int,
) -> list[CampaignResult]:
    specs: list[tuple[int, int, SimConfig, int, int]] = []
    campaign_index = 0
    for max_hop, anchor_count in topologies:
        for slots_per_round in slots:
            for round_count in rounds:
                campaign_seed = seed + campaign_index * 1_000_003
                specs.append(
                    (
                        max_hop,
                        anchor_count,
                        SimConfig(slots_per_round, round_count),
                        trials,
                        campaign_seed,
                    )
                )
                campaign_index += 1
    return run_specs(specs, jobs)


def run_specs(
    specs: Sequence[tuple[int, int, SimConfig, int, int]], jobs: int
) -> list[CampaignResult]:
    if jobs == 1:
        return [run_campaign(*spec) for spec in specs]
    with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as executor:
        futures = [executor.submit(run_campaign, *spec) for spec in specs]
        return [future.result() for future in futures]


def parse_int_range(value: str) -> list[int]:
    if ":" not in value:
        return [int(value)]
    first, last = (int(part) for part in value.split(":", 1))
    if last < first:
        raise argparse.ArgumentTypeError("range end must be at least range start")
    return list(range(first, last + 1))


def write_json(path: Path, results: Sequence[CampaignResult], metadata: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(
            {
                "metadata": metadata,
                "campaigns": [asdict(result) for result in results],
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )


def write_csv(path: Path, results: Sequence[CampaignResult]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    rows = [asdict(result) for result in results]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def self_test() -> None:
    assert diamond_count(1) == 4
    assert diamond_count(2) == 12
    assert diamond_count(3) == 24
    assert diamond_count(4) == 40
    assert len(shell(5)) == 20
    assert ch5_airtime_us(125) == 5445
    assert MAX_RESPONSE_AIRTIME_US == 5363
    assert ACK_AIRTIME_US == 4518
    assert MIN_CELL_US == 19_881
    assert CELL_MARGIN_US == 5_119

    rng = random.Random(7)
    topology = make_topology(5, 50, rng)
    assert len(topology) == 50
    assert max(map(depth, topology)) == 5
    parents = choose_shortest_parents(topology, rng)
    assert all(depth(parent) == depth(node) - 1 for node, parent in parents.items())

    # Depth isolation: only the selected band transmits, only the immediately
    # shallower band receives/ACKs, and RF interference cannot jump a grid edge.
    assert set(nodes_at_depth(topology, 3)).isdisjoint(nodes_at_depth(topology, 2))
    near = Bundle((2, 0), (1, 0), 0, ((2, 0),))
    far = Bundle((3, 0), (2, 0), 0, ((3, 0),))
    assert audible_bundles((1, 0), [near, far]) == [near]
    assert audible_bundles((0, 0), [near, far]) == []
    assert audible_ack_parents((3, 0), [(2, 0), (1, 0)]) == [(2, 0)]

    # A retry never deliberately reuses its immediately previous cell when an
    # alternative exists.
    bundle = Bundle((1, 0), (0, 0), 0, ((1, 0),))
    previous_slots: dict[tuple[Coord, int], int] = {}
    first_slot, _ = _assign_distinct_slots(
        [bundle], 4, previous_slots, random.Random(11)
    )[0]
    second_slot, _ = _assign_distinct_slots(
        [bundle], 4, previous_slots, random.Random(12)
    )[0]
    assert first_slot != second_slot

    # One cell forces all four direct responses to collide at the gateway.
    for seed in range(10):
        result = run_trial(1, 4, SimConfig(1, 1), random.Random(seed))
        assert not result.success
        assert result.response_collisions == 4

    # With retries and fresh random cells, the same topology should recover in
    # almost every trial; this guards the retry state and fresh-cell behavior.
    campaign = run_campaign(1, 4, SimConfig(8, 6), 2_000, 1234)
    assert campaign.success_rate >= 0.999


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--trials", type=int, default=2_000)
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--seed", type=int, default=0xE11E)
    parser.add_argument("--slots", type=parse_int_range, default=parse_int_range("2:16"))
    parser.add_argument("--rounds", type=parse_int_range, default=parse_int_range("1:8"))
    parser.add_argument(
        "--topology",
        action="append",
        metavar="HOPS,ANCHORS",
        help="repeatable; default is full hop 1..4 diamonds plus hop-5 sizes 41..50",
    )
    parser.add_argument(
        "--campaign",
        action="append",
        metavar="HOPS,ANCHORS,SLOTS,ROUNDS",
        help="repeatable targeted campaign; overrides --topology/--slots/--rounds",
    )
    parser.add_argument("--json", type=Path)
    parser.add_argument("--csv", type=Path)
    args = parser.parse_args()

    if args.self_test:
        self_test()
        print("PASS enumeration_slot_sim self-test")
        if not args.json and not args.csv:
            return 0

    if args.trials < 1:
        parser.error("--trials must be positive")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    topologies = topology_matrix()
    if args.topology:
        try:
            topologies = [tuple(map(int, value.split(",", 1))) for value in args.topology]
        except (TypeError, ValueError) as exc:
            parser.error(f"invalid --topology: {exc}")

    if args.campaign:
        try:
            requested = [
                tuple(map(int, value.split(",", 3))) for value in args.campaign
            ]
            if any(len(item) != 4 for item in requested):
                raise ValueError("each campaign needs four comma-separated integers")
        except (TypeError, ValueError) as exc:
            parser.error(f"invalid --campaign: {exc}")
        specs = [
            (
                max_hop,
                anchor_count,
                SimConfig(slots_per_round, round_count),
                args.trials,
                args.seed + index * 1_000_003,
            )
            for index, (max_hop, anchor_count, slots_per_round, round_count)
            in enumerate(requested)
        ]
        results = run_specs(specs, args.jobs)
        topologies = list(dict.fromkeys((item[0], item[1]) for item in requested))
    else:
        results = sweep(
            topologies, args.slots, args.rounds, args.trials, args.seed, args.jobs
        )
    metadata = {
        "model": "depth_banded_compact_channel5_v1",
        "max_response_bytes_without_fcs": MAX_RESPONSE_BYTES,
        "max_response_airtime_us": MAX_RESPONSE_AIRTIME_US,
        "ack_bytes_without_fcs": ACK_BYTES,
        "ack_airtime_us": ACK_AIRTIME_US,
        "ack_turnaround_guard_us": ACK_TURNAROUND_GUARD_US,
        "minimum_complete_cell_us": MIN_CELL_US,
        "cell_us": CELL_US,
        "cell_margin_us": CELL_MARGIN_US,
        "records_per_bundle": RECORDS_PER_BUNDLE,
        "rf_model": "orthogonal one-grid-edge decode and interference",
        "loss_model": "collisions and half-duplex only; no independent RF loss",
        "success_definition": "all records at gateway and every custody ACK complete",
    }
    if args.json:
        write_json(args.json, results, metadata)
    if args.csv:
        write_csv(args.csv, results)

    passing = [result for result in results if result.success_rate >= 0.999]
    print(json.dumps(metadata, sort_keys=True))
    print(f"campaigns={len(results)} passing_raw_99.9={len(passing)}")
    for max_hop, anchor_count in topologies:
        topology_results = [
            result for result in passing
            if result.max_hop == max_hop and result.anchor_count == anchor_count
        ]
        if not topology_results:
            print(f"H{max_hop} N{anchor_count}: no passing configuration")
            continue
        best = min(
            topology_results,
            key=lambda result: (result.interval_s, -result.success_rate),
        )
        print(
            f"H{max_hop} N{anchor_count}: {best.interval_s:.3f}s "
            f"({best.slots_per_round} slots x {best.rounds} rounds) "
            f"success={best.successes}/{best.trials}={best.success_rate:.5f}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
