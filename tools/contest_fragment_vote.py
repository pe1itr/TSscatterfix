#!/usr/bin/env python3
"""Build H.264/H.265 still candidates from repeated TS/PES/NAL fragments.

This is an offline experiment for contest-mode recovery. It does not invent image
content. It extracts codec access units from the video PID, keeps observed headers,
writes one candidate elementary stream per keyframe access unit, and optionally uses
ffmpeg to decode candidate PNGs for inspection.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import csv
import json
import math
import shutil
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

TS_SIZE = 188
SYNC = 0x47
NULL_PID = 0x1FFF
ML_FEATURES = [
    "packet_confidence",
    "packets",
    "slice_bytes",
    "cc_errors",
    "missing_by_cc",
    "out_of_order",
    "bad_pes_starts",
    "low_entropy_packets",
    "high_entropy_packets",
    "idr_bytes",
    "has_idr",
]


@dataclass
class NalUnit:
    nal_type: int
    data: bytes


@dataclass(frozen=True)
class CodecSpec:
    name: str
    extension: str
    stream_types: tuple[int, ...]
    header_types: tuple[int, ...]
    keyframe_types: tuple[int, ...]
    slice_types: tuple[int, ...]
    passthrough_types: tuple[int, ...]


CODECS = {
    "h264": CodecSpec(
        name="h264",
        extension=".h264",
        stream_types=(0x1B,),
        header_types=(7, 8),
        keyframe_types=(5,),
        slice_types=(1, 5),
        passthrough_types=(5, 6, 9),
    ),
    "h265": CodecSpec(
        name="h265",
        extension=".h265",
        stream_types=(0x24,),
        header_types=(32, 33, 34),
        keyframe_types=(19, 20, 21),
        slice_types=tuple(range(0, 32)),
        passthrough_types=(19, 20, 21, 35, 39, 40),
    ),
}

STREAM_TYPE_CODECS = {
    0x02: "mpeg2video",
    0x10: "mpeg4video",
    0x1B: "h264",
    0x24: "h265",
    0x27: "h266",
}

SUPPORTED_STREAM_TYPES = {stream_type for spec in CODECS.values() for stream_type in spec.stream_types}


@dataclass
class AccessUnit:
    source: str
    index: int
    codec: str
    packets: int = 0
    cc_errors: int = 0
    missing_by_cc: int = 0
    out_of_order: int = 0
    confidence_sum: float = 0.0
    confidence_packets: int = 0
    low_entropy_packets: int = 0
    high_entropy_packets: int = 0
    bad_pes_starts: int = 0
    nals: list[NalUnit] = field(default_factory=list)

    @property
    def has_idr(self) -> bool:
        spec = CODECS[self.codec]
        return any(n.nal_type in spec.keyframe_types for n in self.nals)

    @property
    def slice_bytes(self) -> int:
        spec = CODECS[self.codec]
        return sum(len(n.data) for n in self.nals if n.nal_type in spec.slice_types)

    @property
    def packet_confidence(self) -> float:
        if self.confidence_packets == 0:
            return 0.0
        return self.confidence_sum / self.confidence_packets

    @property
    def score(self) -> int:
        confidence_bonus = int((self.packet_confidence - 0.5) * self.packets * 128)
        return (
            self.slice_bytes
            + confidence_bonus
            - self.missing_by_cc * TS_SIZE
            - self.cc_errors * 64
            - self.out_of_order * 128
            - self.bad_pes_starts * 96
        )


def ml_feature_value(au: AccessUnit, name: str) -> float:
    if name == "packet_confidence":
        return au.packet_confidence
    if name == "packets":
        return float(au.packets)
    if name == "slice_bytes":
        return float(au.slice_bytes)
    if name == "cc_errors":
        return float(au.cc_errors)
    if name == "missing_by_cc":
        return float(au.missing_by_cc)
    if name == "out_of_order":
        return float(au.out_of_order)
    if name == "bad_pes_starts":
        return float(au.bad_pes_starts)
    if name == "low_entropy_packets":
        return float(au.low_entropy_packets)
    if name == "high_entropy_packets":
        return float(au.high_entropy_packets)
    if name == "idr_bytes":
        return float(len(idr_data(au)))
    if name == "has_idr":
        return 1.0 if au.has_idr else 0.0
    return 0.0


def load_ml_model(path: Path | None) -> dict | None:
    if path is None:
        return None
    return json.loads(path.read_text())


def ml_score(au: AccessUnit, model: dict | None) -> float:
    if not model:
        return float(au.score)
    values = [1.0]
    for name in model["features"]:
        mean = float(model["means"].get(name, 0.0))
        scale = float(model["scales"].get(name, 1.0)) or 1.0
        values.append((ml_feature_value(au, name) - mean) / scale)
    return sum(float(w) * v for w, v in zip(model["weights"], values))


def parse_ts_packet(pkt: bytes) -> dict[str, int | bool]:
    if len(pkt) != TS_SIZE or pkt[0] != SYNC:
        raise ValueError("invalid TS packet")
    afc = (pkt[3] >> 4) & 0x03
    adaptation_valid = afc != 0
    off = 4
    if afc in (2, 3):
        adaptation_len = pkt[4]
        if 5 + adaptation_len > TS_SIZE:
            adaptation_valid = False
            off = TS_SIZE
        else:
            off += 1 + adaptation_len
    return {
        "tei": bool(pkt[1] & 0x80),
        "pusi": bool(pkt[1] & 0x40),
        "pid": ((pkt[1] & 0x1F) << 8) | pkt[2],
        "scrambling": (pkt[3] >> 6) & 0x03,
        "cc": pkt[3] & 0x0F,
        "adaptation_valid": adaptation_valid,
        "payload_offset": off if afc in (1, 3) and off < TS_SIZE else TS_SIZE,
    }


def payload_entropy(payload: bytes) -> float:
    if not payload:
        return 0.0
    counts = [0] * 256
    for b in payload:
        counts[b] += 1
    entropy = 0.0
    size = len(payload)
    for count in counts:
        if count:
            p = count / size
            entropy -= p * math.log2(p)
    return entropy


def packet_confidence(pkt: bytes, info: dict[str, int | bool], cc_status: str, missing_delta: int) -> tuple[float, str]:
    score = 0.70
    reasons: list[str] = []

    if info["tei"]:
        score -= 0.70
        reasons.append("tei")
    if int(info["scrambling"]) != 0:
        score -= 0.45
        reasons.append("scrambled")
    if not info["adaptation_valid"]:
        score -= 0.45
        reasons.append("bad_adaptation")

    payload_offset = int(info["payload_offset"])
    payload = pkt[payload_offset:] if payload_offset < TS_SIZE else b""
    if not payload:
        score -= 0.20
        reasons.append("no_payload")
    else:
        entropy = payload_entropy(payload)
        if entropy < 1.0:
            score -= 0.20
            reasons.append("low_entropy")
        elif entropy > 7.85:
            score -= 0.10
            reasons.append("high_entropy")
        else:
            score += 0.08

    if info["pusi"] and payload:
        if payload.startswith(b"\x00\x00\x01"):
            score += 0.16
        else:
            score -= 0.30
            reasons.append("bad_pes_start")

    if cc_status == "ok":
        score += 0.12
    elif cc_status == "duplicate":
        score -= 0.25
        reasons.append("duplicate_cc")
    elif cc_status == "missing":
        score -= min(0.55, 0.12 * max(1, missing_delta))
        reasons.append("missing_cc")
    elif cc_status == "out_of_order":
        score -= 0.35
        reasons.append("out_of_order_cc")

    return max(0.0, min(1.0, score)), ",".join(reasons) if reasons else "ok"


def section_from_payload(pkt: bytes, info: dict[str, int | bool]) -> bytes | None:
    off = int(info["payload_offset"])
    if off >= TS_SIZE:
        return None
    if info["pusi"]:
        pointer = pkt[off]
        off += 1 + pointer
    if off + 3 > TS_SIZE:
        return None
    section_len = ((pkt[off + 1] & 0x0F) << 8) | pkt[off + 2]
    end = off + 3 + section_len
    if end > TS_SIZE:
        return None
    return pkt[off:end]


def parse_pat(section: bytes) -> int | None:
    if not section or section[0] != 0x00 or len(section) < 12:
        return None
    section_len = ((section[1] & 0x0F) << 8) | section[2]
    end = 3 + section_len - 4
    pos = 8
    while pos + 4 <= end:
        program = (section[pos] << 8) | section[pos + 1]
        pid = ((section[pos + 2] & 0x1F) << 8) | section[pos + 3]
        if program != 0:
            return pid
        pos += 4
    return None


def pmt_streams(section: bytes) -> list[tuple[int, int]]:
    if not section or section[0] != 0x02 or len(section) < 16:
        return []
    section_len = ((section[1] & 0x0F) << 8) | section[2]
    end = 3 + section_len - 4
    program_info_len = ((section[10] & 0x0F) << 8) | section[11]
    pos = 12 + program_info_len
    streams: list[tuple[int, int]] = []
    while pos + 5 <= end:
        stream_type = section[pos]
        pid = ((section[pos + 1] & 0x1F) << 8) | section[pos + 2]
        es_info_len = ((section[pos + 3] & 0x0F) << 8) | section[pos + 4]
        streams.append((stream_type, pid))
        pos += 5 + es_info_len
    return streams


def parse_pmt(section: bytes, codec_arg: str) -> tuple[int, str, int] | None:
    for stream_type, pid in pmt_streams(section):
        for name, spec in CODECS.items():
            if codec_arg in ("auto", name) and stream_type in spec.stream_types:
                return pid, name, stream_type
    return None


def strip_pes_header(payload: bytes) -> bytes:
    if len(payload) < 9 or payload[:3] != b"\x00\x00\x01":
        return payload
    header_len = payload[8]
    if 9 + header_len <= len(payload):
        return payload[9 + header_len :]
    return b""


def split_nals(es: bytes, codec: str) -> list[NalUnit]:
    starts: list[tuple[int, int]] = []
    i = 0
    while i + 3 < len(es):
        if es[i : i + 3] == b"\x00\x00\x01":
            starts.append((i, 3))
            i += 3
        elif i + 4 < len(es) and es[i : i + 4] == b"\x00\x00\x00\x01":
            starts.append((i, 4))
            i += 4
        else:
            i += 1
    nals: list[NalUnit] = []
    for idx, (pos, length) in enumerate(starts):
        start = pos + length
        end = starts[idx + 1][0] if idx + 1 < len(starts) else len(es)
        if start >= end:
            continue
        if codec == "h265":
            if start + 1 >= end:
                continue
            nal_type = (es[start] >> 1) & 0x3F
        else:
            nal_type = es[start] & 0x1F
        nals.append(NalUnit(nal_type, es[pos:end]))
    return nals


def read_access_units(path: Path, video_pid_arg: int | None, codec_arg: str) -> tuple[int, str, list[AccessUnit], dict[int, bytes]]:
    data = path.read_bytes()
    pmt_pid = None
    video_pid = video_pid_arg
    codec = "h264" if codec_arg == "auto" else codec_arg
    last_cc: dict[int, int] = {}
    current = AccessUnit(source=path.stem, index=0, codec=codec)
    current_pes = bytearray()
    access_units: list[AccessUnit] = []
    best_headers: dict[int, bytes] = {}
    warnings_seen: set[str] = set()

    def warn_once(key: str, message: str) -> None:
        if key in warnings_seen:
            return
        warnings_seen.add(key)
        print(f"[fragment] codec_warning {message}")

    def finish_current() -> None:
        nonlocal current, current_pes
        if current_pes:
            current.nals.extend(split_nals(strip_pes_header(bytes(current_pes)), codec))
            current_pes.clear()
        for nal in current.nals:
            if nal.nal_type in CODECS[codec].header_types and len(nal.data) > len(best_headers.get(nal.nal_type, b"")):
                best_headers[nal.nal_type] = nal.data
        if current.packets or current.nals:
            access_units.append(current)
        current = AccessUnit(source=path.stem, index=current.index + 1, codec=codec)

    for packet_index in range(len(data) // TS_SIZE):
        pkt = data[packet_index * TS_SIZE : (packet_index + 1) * TS_SIZE]
        try:
            info = parse_ts_packet(pkt)
        except ValueError:
            continue
        pid = int(info["pid"])
        payload_offset = int(info["payload_offset"])
        has_payload = payload_offset < TS_SIZE

        if pid == 0 and has_payload:
            section = section_from_payload(pkt, info)
            if section:
                pmt_pid = parse_pat(section) or pmt_pid
        elif pmt_pid is not None and pid == pmt_pid and has_payload:
            section = section_from_payload(pkt, info)
            if section:
                for stream_type, stream_pid in pmt_streams(section):
                    observed_codec = STREAM_TYPE_CODECS.get(stream_type)
                    if stream_type in SUPPORTED_STREAM_TYPES:
                        supported_codec = next(name for name, spec in CODECS.items() if stream_type in spec.stream_types)
                        if video_pid == stream_pid and codec_arg != "auto" and codec_arg != supported_codec:
                            warn_once(
                                f"mismatch:{stream_pid}:{stream_type}:{codec_arg}",
                                f"pid=0x{stream_pid:04x} stream_type=0x{stream_type:02x} "
                                f"pmt_codec={supported_codec} selected_codec={codec_arg} hint=--codec {supported_codec}",
                            )
                        elif video_pid is None and codec_arg != "auto" and codec_arg != supported_codec:
                            warn_once(
                                f"ignored-supported:{stream_pid}:{stream_type}:{codec_arg}",
                                f"pid=0x{stream_pid:04x} stream_type=0x{stream_type:02x} "
                                f"pmt_codec={supported_codec} ignored_by_selected_codec={codec_arg} hint=--codec {supported_codec}",
                            )
                    elif observed_codec:
                        warn_once(
                            f"unsupported:{stream_pid}:{stream_type}",
                            f"pid=0x{stream_pid:04x} stream_type=0x{stream_type:02x} "
                            f"pmt_codec={observed_codec} supported_codecs=h264,h265",
                        )
                parsed = parse_pmt(section, codec_arg)
                if parsed:
                    parsed_pid, parsed_codec, _stream_type = parsed
                    if video_pid is None:
                        video_pid = parsed_pid
                    if video_pid == parsed_pid and codec_arg == "auto":
                        codec = parsed_codec
                        current.codec = codec

        if video_pid is None or pid != video_pid or not has_payload or pid == NULL_PID:
            continue

        cc = int(info["cc"])
        cc_status = "ok"
        missing_delta = 0
        if pid in last_cc:
            expected = (last_cc[pid] + 1) & 0x0F
            if cc != expected:
                delta = (cc - expected) & 0x0F
                if 1 <= delta <= 8:
                    cc_status = "missing"
                    missing_delta = delta
                elif cc != last_cc[pid]:
                    cc_status = "out_of_order"
                else:
                    cc_status = "duplicate"
        last_cc[pid] = cc

        if info["pusi"]:
            finish_current()
        if cc_status != "ok":
            current.cc_errors += 1
            if cc_status == "missing":
                current.missing_by_cc += missing_delta
            elif cc_status == "out_of_order":
                current.out_of_order += 1

        confidence, reasons = packet_confidence(pkt, info, cc_status, missing_delta)
        current.confidence_sum += confidence
        current.confidence_packets += 1
        if "low_entropy" in reasons:
            current.low_entropy_packets += 1
        if "high_entropy" in reasons:
            current.high_entropy_packets += 1
        if "bad_pes_start" in reasons:
            current.bad_pes_starts += 1
        current.packets += 1
        current_pes.extend(pkt[payload_offset:])

    finish_current()
    if video_pid is None:
        raise SystemExit("could not detect video PID; pass --video-pid 0xNNNN")
    return video_pid, codec, access_units, best_headers


def write_candidates(access_units: list[AccessUnit], headers: dict[int, bytes], out_dir: Path, max_candidates: int, model: dict | None) -> list[Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    idr_units = sorted((au for au in access_units if au.has_idr), key=lambda au: ml_score(au, model), reverse=True)
    paths: list[Path] = []
    for rank, au in enumerate(idr_units[:max_candidates], 1):
        spec = CODECS[au.codec]
        path = out_dir / f"candidate_{rank:02d}_{au.source}_au_{au.index:03d}{spec.extension}"
        with path.open("wb") as f:
            for header_type in spec.header_types:
                if headers.get(header_type):
                    f.write(headers[header_type])
            for nal in au.nals:
                if nal.nal_type in spec.passthrough_types:
                    f.write(nal.data)
        paths.append(path)
        print(
            f"[fragment] candidate={path.name} source={au.source} au={au.index} score={au.score} ml={ml_score(au, model):.3f} conf={au.packet_confidence:.3f} packets={au.packets} "
            f"cc_errors={au.cc_errors} missing={au.missing_by_cc} out_of_order={au.out_of_order} "
            f"bad_pes={au.bad_pes_starts} entropy_low={au.low_entropy_packets} entropy_high={au.high_entropy_packets} slice_bytes={au.slice_bytes}"
        )
    return paths


def idr_data(au: AccessUnit) -> bytes:
    spec = CODECS[au.codec]
    for nal in au.nals:
        if nal.nal_type in spec.keyframe_types:
            return nal.data
    return b""


def au_quality(au: AccessUnit, model: dict | None) -> int:
    base = ml_score(au, model) * 1024 if model else au.score
    return max(1, int((base + 4096) * (0.5 + au.packet_confidence)))


def write_voted_candidates(access_units: list[AccessUnit], headers: dict[int, bytes], out_dir: Path, model: dict | None) -> list[Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    idr_units = [au for au in access_units if au.has_idr and idr_data(au)]
    groups: dict[tuple[str, int], list[AccessUnit]] = defaultdict(list)
    for au in idr_units:
        # TS damage tends to truncate by whole packets. Group by approximate
        # fragment length so unrelated parts of the frame are not overlaid.
        groups[(au.codec, (len(idr_data(au)) + 93) // 188)].append(au)

    paths: list[Path] = []
    rank = 1
    for (_codec, bucket), group in sorted(groups.items(), key=lambda item: max(ml_score(au, model) for au in item[1]), reverse=True):
        if len(group) < 2:
            continue
        spec = CODECS[group[0].codec]
        max_len = max(len(idr_data(au)) for au in group)
        voted = bytearray(max_len)
        support = 0
        for pos in range(max_len):
            counts: dict[int, int] = {}
            fallback = 0
            fallback_weight = -1
            for au in group:
                data = idr_data(au)
                if pos >= len(data):
                    continue
                weight = au_quality(au, model)
                value = data[pos]
                counts[value] = counts.get(value, 0) + weight
                if weight > fallback_weight:
                    fallback = value
                    fallback_weight = weight
            if counts:
                value, weight = max(counts.items(), key=lambda item: item[1])
                voted[pos] = value
                if weight > fallback_weight:
                    support += 1
            else:
                voted[pos] = fallback

        sources = "_".join(f"{au.source}-au{au.index:03d}" for au in group)
        if len(sources) > 120:
            sources = sources[:120]
        path = out_dir / f"voted_{rank:02d}_bucket_{bucket:02d}_{sources}{spec.extension}"
        with path.open("wb") as f:
            for header_type in spec.header_types:
                if headers.get(header_type):
                    f.write(headers[header_type])
            f.write(bytes(voted))
        paths.append(path)
        print(
            f"[vote] candidate={path.name} group_size={len(group)} bucket={bucket} idr_len={len(voted)} "
            f"members=" + ",".join(
                f"{au.source}:au{au.index}:len{len(idr_data(au))}:score{au.score}:conf{au.packet_confidence:.3f}"
                for au in group
            )
        )
        rank += 1
    if not paths:
        print("[vote] no groups with at least two similarly sized IDR slices")
    return paths


def decode_candidates(paths: list[Path], out_dir: Path) -> None:
    if shutil.which("ffmpeg") is None:
        print("[fragment] ffmpeg=missing decode=skipped")
        return
    for path in paths:
        png = path.with_suffix(".png")
        cmd = ["ffmpeg", "-hide_banner", "-y", "-err_detect", "ignore_err", "-i", str(path), "-frames:v", "1", str(png)]
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        status = "ok" if proc.returncode == 0 and png.exists() else "failed"
        print(f"[fragment] decode={status} input={path.name} output={png.name}")


def write_features_csv(access_units: list[AccessUnit], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(
            [
                "source",
                "access_unit",
                "has_idr",
                "score",
                "packet_confidence",
                "packets",
                "slice_bytes",
                "cc_errors",
                "missing_by_cc",
                "out_of_order",
                "bad_pes_starts",
                "low_entropy_packets",
                "high_entropy_packets",
                "nal_types",
                "idr_bytes",
            ]
        )
        for au in access_units:
            writer.writerow(
                [
                    au.source,
                    au.index,
                    int(au.has_idr),
                    au.score,
                    f"{au.packet_confidence:.6f}",
                    au.packets,
                    au.slice_bytes,
                    au.cc_errors,
                    au.missing_by_cc,
                    au.out_of_order,
                    au.bad_pes_starts,
                    au.low_entropy_packets,
                    au.high_entropy_packets,
                    " ".join(str(n.nal_type) for n in au.nals),
                    len(idr_data(au)),
                ]
            )
    print(f"[features] csv={path} rows={len(access_units)}")


def header_status(codec: str, headers: dict[int, bytes]) -> str:
    if codec == "h265":
        return (
            f"vps={'yes' if headers.get(32) else 'no'} "
            f"sps={'yes' if headers.get(33) else 'no'} "
            f"pps={'yes' if headers.get(34) else 'no'}"
        )
    return f"sps={'yes' if headers.get(7) else 'no'} pps={'yes' if headers.get(8) else 'no'}"


def main() -> int:
    parser = argparse.ArgumentParser(description="Create H.264/H.265 keyframe candidates from repeated contest fragments.")
    parser.add_argument("--input", required=True, action="append", type=Path, help="Input TS capture. Can be passed multiple times.")
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--video-pid", type=lambda s: int(s, 0))
    parser.add_argument("--codec", choices=("auto", "h264", "h265"), default="auto")
    parser.add_argument("--max-candidates", type=int, default=12)
    parser.add_argument("--decode", action="store_true", help="Decode candidates to PNG with ffmpeg")
    parser.add_argument("--vote", action="store_true", help="Also write byte-voted candidates from similarly sized IDR slices")
    parser.add_argument("--features-csv", type=Path, help="Write access-unit confidence features for ML experiments")
    parser.add_argument("--ml-model", type=Path, help="Use a trained JSON model for ranking and vote weighting")
    args = parser.parse_args()
    model = load_ml_model(args.ml_model)
    if model:
        print(f"[ml] model={args.ml_model} kind={model.get('kind')} rows={model.get('train_rows')}")

    all_access_units: list[AccessUnit] = []
    best_headers_by_codec: dict[str, dict[int, bytes]] = {name: {} for name in CODECS}
    video_pids: set[int] = set()
    codecs: set[str] = set()
    for input_path in args.input:
        video_pid, codec, access_units, headers = read_access_units(input_path, args.video_pid, args.codec)
        video_pids.add(video_pid)
        codecs.add(codec)
        all_access_units.extend(access_units)
        best_headers = best_headers_by_codec[codec]
        for nal_type, data in headers.items():
            if len(data) > len(best_headers.get(nal_type, b"")):
                best_headers[nal_type] = data
        idr_count = sum(1 for au in access_units if au.has_idr)
        print(
            f"[fragment] input={input_path} codec={codec} video_pid=0x{video_pid:04x} access_units={len(access_units)} "
            f"idr_units={idr_count} key_units={idr_count} {header_status(codec, headers)}"
        )

    idr_count = sum(1 for au in all_access_units if au.has_idr)
    video_pid_text = ",".join(f"0x{pid:04x}" for pid in sorted(video_pids))
    codec_text = ",".join(sorted(codecs))
    primary_codec = sorted(codecs)[0] if codecs else "h264"
    primary_headers = best_headers_by_codec[primary_codec]
    print(
        f"[fragment] combined codecs={codec_text} video_pids={video_pid_text} access_units={len(all_access_units)} "
        f"idr_units={idr_count} key_units={idr_count} {header_status(primary_codec, primary_headers)}"
    )
    candidate_headers = best_headers_by_codec
    candidates: list[Path] = []
    for codec in sorted(codecs):
        codec_units = [au for au in all_access_units if au.codec == codec]
        candidates.extend(write_candidates(codec_units, candidate_headers[codec], args.output_dir, args.max_candidates, model))
    if args.vote:
        for codec in sorted(codecs):
            codec_units = [au for au in all_access_units if au.codec == codec]
            candidates.extend(write_voted_candidates(codec_units, candidate_headers[codec], args.output_dir, model))
    if args.features_csv:
        write_features_csv(all_access_units, args.features_csv)
    if args.decode:
        decode_candidates(candidates, args.output_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
