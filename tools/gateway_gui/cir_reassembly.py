"""Strict host-side reassembly model for mesh click-report CIR fragments."""

from __future__ import annotations

from dataclasses import dataclass, field
import math

from .protocol import (
    MSG_CLICK_REPORT,
    Packet,
    click_report_session_id,
    TLV_ANCHOR_ID,
    TLV_CLICKER_ID,
    TLV_DIAG_FRAGMENT_COUNT,
    TLV_DIAG_FRAGMENT_INDEX,
    TLV_EVENT_SEQ,
    TLV_TIMESTAMP_MS,
    TLV_UWB_CIR_BYTE_OFFSET,
    TLV_UWB_CIR_FIRST_PATH_INDEX,
    TLV_UWB_CIR_FULL_CHUNK,
    TLV_UWB_CIR_START_INDEX,
    TLV_UWB_CIR_TOTAL_BYTES,
)


FLAG_DIAGNOSTIC = 0x10
CIR_SAMPLE_BYTES = 6
CIR_WINDOW_SAMPLES = 192
CIR_WINDOW_BYTES = CIR_SAMPLE_BYTES * CIR_WINDOW_SAMPLES
CIR_ACCUMULATOR_SAMPLES = 2048
CIR_FRAGMENT_COUNT = 2

CIR_FRAGMENT_TLV_TYPES = {
    TLV_UWB_CIR_BYTE_OFFSET,
    TLV_UWB_CIR_TOTAL_BYTES,
    TLV_UWB_CIR_FIRST_PATH_INDEX,
    TLV_UWB_CIR_START_INDEX,
    TLV_UWB_CIR_FULL_CHUNK,
}


@dataclass(frozen=True, order=True)
class CirAssemblyKey:
    clicker_id: int
    anchor_id: int
    event_seq: int


@dataclass(frozen=True)
class CirFragment:
    key: CirAssemblyKey
    timestamp_ms: int
    fragment_index: int
    fragment_count: int
    byte_offset: int
    total_bytes: int
    first_path_index: int
    start_index: int
    chunk: bytes

    @property
    def end_offset(self) -> int:
        return self.byte_offset + len(self.chunk)


@dataclass(frozen=True)
class CirSample:
    window_index: int
    absolute_index: int
    byte_offset: int
    raw: bytes
    real: int
    imaginary: int
    magnitude: float


@dataclass(frozen=True)
class CirAssemblyView:
    key: CirAssemblyKey
    timestamp_ms: int
    fragment_count: int
    total_bytes: int
    first_path_index: int
    start_index: int
    received_fragment_indices: tuple[int, ...]
    missing_fragment_indices: tuple[int, ...]
    coverage: tuple[tuple[int, int], ...]
    gaps: tuple[tuple[int, int], ...]
    bytes_received: int
    errors: tuple[str, ...]
    complete: bool
    raw: bytes | None
    samples: tuple[CirSample, ...]

    @property
    def state(self) -> str:
        if self.errors:
            return "malformed"
        return "complete" if self.complete else "incomplete"


@dataclass(frozen=True)
class CirIngestResult:
    key: CirAssemblyKey | None
    accepted: bool
    errors: tuple[str, ...]
    view: CirAssemblyView | None


@dataclass
class _CirAssembly:
    key: CirAssemblyKey
    timestamp_ms: int
    fragment_count: int
    total_bytes: int
    first_path_index: int
    start_index: int
    fragments: dict[int, CirFragment] = field(default_factory=dict)
    errors: list[str] = field(default_factory=list)


def is_cir_fragment_candidate(packet: Packet) -> bool:
    return any(tlv.type_id in CIR_FRAGMENT_TLV_TYPES for tlv in packet.tlvs)


def _signed24(raw: bytes) -> int:
    return int.from_bytes(raw, "little", signed=True)


def decode_accumulator_sample(raw: bytes, window_index: int, start_index: int) -> CirSample:
    """Decode one DWM3000 accumulator sample in device byte order."""
    if len(raw) != CIR_SAMPLE_BYTES:
        raise ValueError(f"expected {CIR_SAMPLE_BYTES} CIR bytes, received {len(raw)}")
    real = _signed24(raw[:3])
    imaginary = _signed24(raw[3:])
    return CirSample(
        window_index=window_index,
        absolute_index=start_index + window_index,
        byte_offset=window_index * CIR_SAMPLE_BYTES,
        raw=raw,
        real=real,
        imaginary=imaginary,
        magnitude=math.hypot(real, imaginary),
    )


def _format_ranges(ranges: tuple[tuple[int, int], ...]) -> str:
    return ", ".join(f"[{start},{end})" for start, end in ranges)


class CirReassembler:
    """Accumulates fragments without filling gaps or accepting overlaps."""

    def __init__(self) -> None:
        self._assemblies: dict[CirAssemblyKey, _CirAssembly] = {}

    def clear(self) -> None:
        self._assemblies.clear()

    def view(self, key: CirAssemblyKey) -> CirAssemblyView | None:
        assembly = self._assemblies.get(key)
        return None if assembly is None else self._view(assembly)

    def ingest(self, packet: Packet) -> CirIngestResult | None:
        if not is_cir_fragment_candidate(packet):
            return None

        fragment, parse_errors = self._parse_fragment(packet)
        if fragment is None:
            return CirIngestResult(None, False, tuple(parse_errors), None)

        assembly = self._assemblies.get(fragment.key)
        if assembly is None:
            assembly = _CirAssembly(
                key=fragment.key,
                timestamp_ms=fragment.timestamp_ms,
                fragment_count=fragment.fragment_count,
                total_bytes=fragment.total_bytes,
                first_path_index=fragment.first_path_index,
                start_index=fragment.start_index,
            )
            self._assemblies[fragment.key] = assembly

        new_errors = list(parse_errors)
        new_errors.extend(self._metadata_errors(assembly, fragment))
        if not new_errors:
            new_errors.extend(self._placement_errors(assembly, fragment))

        accepted = not new_errors
        if accepted:
            assembly.fragments[fragment.fragment_index] = fragment
        for error in new_errors:
            self._add_error(assembly, error)
        self._note_terminal_gaps(assembly)
        return CirIngestResult(
            fragment.key,
            accepted,
            tuple(new_errors),
            self._view(assembly),
        )

    @staticmethod
    def _single_value(packet: Packet, type_id: int, name: str, errors: list[str]) -> int | bytes | None:
        values = [tlv for tlv in packet.tlvs if tlv.type_id == type_id]
        if not values:
            errors.append(f"missing required {name} TLV")
            return None
        if len(values) != 1:
            errors.append(f"expected one {name} TLV, received {len(values)}")
            return None
        value = values[0]
        if value.truncated or value.decode_error:
            errors.append(f"invalid {name} TLV: {value.display}")
            return None
        if isinstance(value.decoded, (int, bytes)):
            return value.decoded
        errors.append(f"invalid {name} TLV value type")
        return None

    @staticmethod
    def _chunk_bytes(packet: Packet, errors: list[str]) -> bytes | None:
        values = [tlv for tlv in packet.tlvs if tlv.type_id == TLV_UWB_CIR_FULL_CHUNK]
        if not values:
            errors.append("missing required UWB_CIR_FULL_CHUNK TLV")
            return None
        chunks: list[bytes] = []
        for index, value in enumerate(values):
            if value.truncated or value.decode_error:
                errors.append(
                    f"invalid UWB_CIR_FULL_CHUNK TLV occurrence {index}: {value.display}"
                )
                continue
            if not isinstance(value.decoded, bytes):
                errors.append(
                    f"invalid UWB_CIR_FULL_CHUNK TLV occurrence {index} value type"
                )
                continue
            if not value.decoded:
                errors.append(f"UWB_CIR_FULL_CHUNK TLV occurrence {index} must not be empty")
                continue
            chunks.append(value.decoded)
        if len(chunks) != len(values):
            return None
        return b"".join(chunks)

    def _parse_fragment(self, packet: Packet) -> tuple[CirFragment | None, list[str]]:
        errors: list[str] = []
        if packet.msg_type != MSG_CLICK_REPORT:
            errors.append(f"CIR fragment must be MSG_CLICK_REPORT, got 0x{packet.msg_type:02x}")
        if (packet.flags & FLAG_DIAGNOSTIC) == 0:
            errors.append("CIR fragment is missing FLAG_DIAGNOSTIC")

        clicker_id = self._single_value(packet, TLV_CLICKER_ID, "CLICKER_ID", errors)
        anchor_id = self._single_value(packet, TLV_ANCHOR_ID, "ANCHOR_ID", errors)
        event_seq = self._single_value(packet, TLV_EVENT_SEQ, "EVENT_SEQ", errors)
        timestamp_ms = self._single_value(packet, TLV_TIMESTAMP_MS, "TIMESTAMP_MS", errors)
        fragment_index = self._single_value(
            packet, TLV_DIAG_FRAGMENT_INDEX, "DIAG_FRAGMENT_INDEX", errors
        )
        fragment_count = self._single_value(
            packet, TLV_DIAG_FRAGMENT_COUNT, "DIAG_FRAGMENT_COUNT", errors
        )
        byte_offset = self._single_value(
            packet, TLV_UWB_CIR_BYTE_OFFSET, "UWB_CIR_BYTE_OFFSET", errors
        )
        total_bytes = self._single_value(
            packet, TLV_UWB_CIR_TOTAL_BYTES, "UWB_CIR_TOTAL_BYTES", errors
        )
        first_path_index = self._single_value(
            packet, TLV_UWB_CIR_FIRST_PATH_INDEX, "UWB_CIR_FIRST_PATH_INDEX", errors
        )
        start_index = self._single_value(
            packet, TLV_UWB_CIR_START_INDEX, "UWB_CIR_START_INDEX", errors
        )
        chunk = self._chunk_bytes(packet, errors)

        scalar_values = (
            clicker_id,
            anchor_id,
            event_seq,
            timestamp_ms,
            fragment_index,
            fragment_count,
            byte_offset,
            total_bytes,
            first_path_index,
            start_index,
        )
        if not all(isinstance(value, int) for value in scalar_values) or not isinstance(chunk, bytes):
            return None, errors

        assert isinstance(clicker_id, int)
        assert isinstance(anchor_id, int)
        assert isinstance(event_seq, int)
        assert isinstance(timestamp_ms, int)
        assert isinstance(fragment_index, int)
        assert isinstance(fragment_count, int)
        assert isinstance(byte_offset, int)
        assert isinstance(total_bytes, int)
        assert isinstance(first_path_index, int)
        assert isinstance(start_index, int)

        key = CirAssemblyKey(clicker_id, anchor_id, event_seq)
        fragment = CirFragment(
            key=key,
            timestamp_ms=timestamp_ms,
            fragment_index=fragment_index,
            fragment_count=fragment_count,
            byte_offset=byte_offset,
            total_bytes=total_bytes,
            first_path_index=first_path_index,
            start_index=start_index,
            chunk=chunk,
        )
        errors.extend(self._fragment_contract_errors(packet, fragment))
        return fragment, errors

    @staticmethod
    def _fragment_contract_errors(packet: Packet, fragment: CirFragment) -> list[str]:
        errors: list[str] = []
        if fragment.key.clicker_id == 0:
            errors.append("CLICKER_ID must be non-zero")
        if fragment.key.anchor_id == 0:
            errors.append("ANCHOR_ID must be non-zero")
        if fragment.key.clicker_id == fragment.key.anchor_id:
            errors.append("CLICKER_ID and ANCHOR_ID must differ")
        if fragment.key.event_seq == 0:
            errors.append("EVENT_SEQ must be non-zero")
        if packet.src_id != fragment.key.anchor_id:
            errors.append(
                f"packet source {packet.src_id:#018x} does not match ANCHOR_ID "
                f"{fragment.key.anchor_id:#018x}"
            )
        expected_session_id = click_report_session_id(
            fragment.key.clicker_id, fragment.key.event_seq
        )
        if packet.session_id != expected_session_id:
            errors.append(
                f"packet session {packet.session_id} does not bind CLICKER_ID "
                f"{fragment.key.clicker_id:#018x} and EVENT_SEQ {fragment.key.event_seq}"
            )
        if fragment.fragment_count != CIR_FRAGMENT_COUNT:
            errors.append(
                f"DIAG_FRAGMENT_COUNT must be {CIR_FRAGMENT_COUNT}, got "
                f"{fragment.fragment_count}"
            )
        if fragment.fragment_count == 0:
            errors.append("DIAG_FRAGMENT_COUNT must be non-zero")
        elif fragment.fragment_index >= fragment.fragment_count:
            errors.append(
                f"fragment index {fragment.fragment_index} is outside count {fragment.fragment_count}"
            )
        if not fragment.chunk:
            errors.append("UWB_CIR_FULL_CHUNK must not be empty")
        if fragment.total_bytes != CIR_WINDOW_BYTES:
            errors.append(
                f"total bytes {fragment.total_bytes} does not match the 192-sample window "
                f"({CIR_WINDOW_BYTES} bytes)"
            )
        if fragment.total_bytes == 0:
            errors.append("UWB_CIR_TOTAL_BYTES must be non-zero")
        elif fragment.total_bytes % CIR_SAMPLE_BYTES != 0:
            errors.append(
                f"total bytes {fragment.total_bytes} is not divisible by {CIR_SAMPLE_BYTES}"
            )
        if fragment.byte_offset >= fragment.total_bytes:
            errors.append(
                f"byte offset {fragment.byte_offset} is outside total {fragment.total_bytes}"
            )
        elif fragment.end_offset > fragment.total_bytes:
            errors.append(
                f"chunk [{fragment.byte_offset},{fragment.end_offset}) exceeds total "
                f"{fragment.total_bytes}"
            )
        if fragment.fragment_index == 0 and fragment.byte_offset != 0:
            errors.append(
                f"fragment 0 must start at byte offset 0, got {fragment.byte_offset}"
            )
        if (
            fragment.fragment_count > 0
            and fragment.fragment_index == fragment.fragment_count - 1
            and fragment.end_offset != fragment.total_bytes
        ):
            errors.append(
                f"final fragment must end at total byte {fragment.total_bytes}, got "
                f"{fragment.end_offset}"
            )

        sample_count = fragment.total_bytes // CIR_SAMPLE_BYTES if fragment.total_bytes else 0
        if fragment.start_index >= CIR_ACCUMULATOR_SAMPLES:
            errors.append(f"start index {fragment.start_index} is outside the DW3000 accumulator")
        if fragment.first_path_index >= CIR_ACCUMULATOR_SAMPLES:
            errors.append(
                f"first-path index {fragment.first_path_index} is outside the DW3000 accumulator"
            )
        if sample_count and fragment.start_index + sample_count > CIR_ACCUMULATOR_SAMPLES:
            errors.append(
                f"sample window [{fragment.start_index},{fragment.start_index + sample_count}) "
                f"exceeds accumulator size {CIR_ACCUMULATOR_SAMPLES}"
            )
        if sample_count and not (
            fragment.start_index
            <= fragment.first_path_index
            < fragment.start_index + sample_count
        ):
            errors.append(
                f"first-path index {fragment.first_path_index} is outside sample window "
                f"[{fragment.start_index},{fragment.start_index + sample_count})"
            )
        return errors

    @staticmethod
    def _metadata_errors(assembly: _CirAssembly, fragment: CirFragment) -> list[str]:
        errors: list[str] = []
        for name, expected, received in (
            ("TIMESTAMP_MS", assembly.timestamp_ms, fragment.timestamp_ms),
            ("DIAG_FRAGMENT_COUNT", assembly.fragment_count, fragment.fragment_count),
            ("UWB_CIR_TOTAL_BYTES", assembly.total_bytes, fragment.total_bytes),
            ("UWB_CIR_FIRST_PATH_INDEX", assembly.first_path_index, fragment.first_path_index),
            ("UWB_CIR_START_INDEX", assembly.start_index, fragment.start_index),
        ):
            if received != expected:
                errors.append(
                    f"fragment {fragment.fragment_index} metadata mismatch for {name}: "
                    f"expected {expected}, received {received}"
                )
        return errors

    @staticmethod
    def _placement_errors(assembly: _CirAssembly, fragment: CirFragment) -> list[str]:
        if fragment.fragment_index in assembly.fragments:
            return [f"duplicate fragment index {fragment.fragment_index}"]
        for existing in assembly.fragments.values():
            if max(existing.byte_offset, fragment.byte_offset) < min(
                existing.end_offset, fragment.end_offset
            ):
                return [
                    f"fragment {fragment.fragment_index} range "
                    f"[{fragment.byte_offset},{fragment.end_offset}) overlaps fragment "
                    f"{existing.fragment_index} range "
                    f"[{existing.byte_offset},{existing.end_offset})"
                ]
            if (
                fragment.fragment_index < existing.fragment_index
                and fragment.byte_offset >= existing.byte_offset
            ) or (
                fragment.fragment_index > existing.fragment_index
                and fragment.byte_offset <= existing.byte_offset
            ):
                return [
                    f"fragment index order conflicts with byte offsets: fragment "
                    f"{fragment.fragment_index} starts at {fragment.byte_offset}, fragment "
                    f"{existing.fragment_index} starts at {existing.byte_offset}"
                ]
        return []

    @staticmethod
    def _add_error(assembly: _CirAssembly, error: str) -> None:
        if error not in assembly.errors:
            assembly.errors.append(error)

    def _note_terminal_gaps(self, assembly: _CirAssembly) -> None:
        if len(assembly.fragments) != assembly.fragment_count:
            return
        gaps = self._gaps(assembly)
        if gaps:
            self._add_error(
                assembly,
                f"all {assembly.fragment_count} fragment indices received but byte coverage has "
                f"gaps: {_format_ranges(gaps)}",
            )

    @staticmethod
    def _coverage(assembly: _CirAssembly) -> tuple[tuple[int, int], ...]:
        return tuple(
            sorted(
                (fragment.byte_offset, fragment.end_offset)
                for fragment in assembly.fragments.values()
            )
        )

    @classmethod
    def _gaps(cls, assembly: _CirAssembly) -> tuple[tuple[int, int], ...]:
        gaps: list[tuple[int, int]] = []
        cursor = 0
        for start, end in cls._coverage(assembly):
            if start > cursor:
                gaps.append((cursor, start))
            cursor = max(cursor, end)
        if cursor < assembly.total_bytes:
            gaps.append((cursor, assembly.total_bytes))
        return tuple(gaps)

    @staticmethod
    def _decode_samples(raw: bytes, start_index: int) -> tuple[CirSample, ...]:
        samples: list[CirSample] = []
        for byte_offset in range(0, len(raw), CIR_SAMPLE_BYTES):
            sample_raw = raw[byte_offset:byte_offset + CIR_SAMPLE_BYTES]
            window_index = byte_offset // CIR_SAMPLE_BYTES
            samples.append(decode_accumulator_sample(sample_raw, window_index, start_index))
        return tuple(samples)

    def _view(self, assembly: _CirAssembly) -> CirAssemblyView:
        received = tuple(sorted(assembly.fragments))
        missing = tuple(
            index for index in range(assembly.fragment_count) if index not in assembly.fragments
        )
        coverage = self._coverage(assembly)
        gaps = self._gaps(assembly)
        bytes_received = sum(len(fragment.chunk) for fragment in assembly.fragments.values())
        complete = (
            not assembly.errors
            and not missing
            and not gaps
            and bytes_received == assembly.total_bytes
        )
        raw: bytes | None = None
        samples: tuple[CirSample, ...] = ()
        if complete:
            assembled = bytearray(assembly.total_bytes)
            for fragment in assembly.fragments.values():
                assembled[fragment.byte_offset:fragment.end_offset] = fragment.chunk
            raw = bytes(assembled)
            samples = self._decode_samples(raw, assembly.start_index)
        return CirAssemblyView(
            key=assembly.key,
            timestamp_ms=assembly.timestamp_ms,
            fragment_count=assembly.fragment_count,
            total_bytes=assembly.total_bytes,
            first_path_index=assembly.first_path_index,
            start_index=assembly.start_index,
            received_fragment_indices=received,
            missing_fragment_indices=missing,
            coverage=coverage,
            gaps=gaps,
            bytes_received=bytes_received,
            errors=tuple(assembly.errors),
            complete=complete,
            raw=raw,
            samples=samples,
        )
