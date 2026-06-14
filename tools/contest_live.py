#!/usr/bin/env python3
"""Live contest recovery prototype.

Reads MPEG-TS bytes from stdin into a rolling capture, periodically runs the
existing fragment/ML candidate path, and streams the current best frame to
ffmpeg from memory when UDP output is enabled.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

from PIL import Image
from PIL import ImageStat

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_DIR = SCRIPT_DIR.parent
YELLOW = "\033[38;5;226m"
RESET = "\033[0m"


class SharedFrame:
    def __init__(self, size: tuple[int, int]) -> None:
        self.width, self.height = size
        self._frame = Image.new("RGB", size, (0, 0, 0)).tobytes()
        self._version = 0
        self._lock = threading.Lock()

    def update_from_png(self, path: Path) -> int:
        with Image.open(path) as im:
            frame = im.convert("RGB").resize((self.width, self.height), Image.Resampling.BILINEAR).tobytes()
        with self._lock:
            self._frame = frame
            self._version += 1
            return self._version

    def get(self) -> tuple[bytes, int]:
        with self._lock:
            return self._frame, self._version


def run(cmd: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def parse_size(value: str) -> tuple[int, int]:
    try:
        width, height = value.lower().split("x", 1)
        parsed = (int(width), int(height))
    except ValueError as exc:
        raise argparse.ArgumentTypeError("size must be WIDTHxHEIGHT") from exc
    if parsed[0] <= 0 or parsed[1] <= 0:
        raise argparse.ArgumentTypeError("size must be positive")
    return parsed


def parse_candidate_scores(log: str) -> list[tuple[float, str]]:
    scores: list[tuple[float, str]] = []
    for line in log.splitlines():
        if not line.startswith("[fragment] candidate="):
            continue
        parts = line.split()
        name = ""
        score = None
        for part in parts:
            if part.startswith("candidate="):
                name = part.split("=", 1)[1]
            elif part.startswith("ml="):
                try:
                    score = float(part.split("=", 1)[1])
                except ValueError:
                    pass
            elif part.startswith("score=") and score is None:
                try:
                    score = float(part.split("=", 1)[1])
                except ValueError:
                    pass
        if name and score is not None:
            scores.append((score, name))
    return sorted(scores, reverse=True)


def summarize_analysis_log(log: str) -> str:
    access_units = None
    idr_units = None
    key_units = None
    codec = None
    vps = None
    sps = None
    pps = None
    candidates = 0
    voted_candidates = 0
    decode_ok = 0
    decode_failed = 0
    features_rows = None
    ffmpeg_missing = False
    vote_no_groups = False
    decode_ready = None
    decode_ready_reason = ""
    codec_warnings: list[str] = []
    error = ""
    low_quality = ""

    for line in log.splitlines():
        if line.startswith("[fragment] combined "):
            for part in line.split():
                if part.startswith("access_units="):
                    access_units = part.split("=", 1)[1]
                elif part.startswith("idr_units="):
                    idr_units = part.split("=", 1)[1]
                elif part.startswith("key_units="):
                    key_units = part.split("=", 1)[1]
                elif part.startswith("codecs="):
                    codec = part.split("=", 1)[1]
                elif part.startswith("vps="):
                    vps = part.split("=", 1)[1]
                elif part.startswith("sps="):
                    sps = part.split("=", 1)[1]
                elif part.startswith("pps="):
                    pps = part.split("=", 1)[1]
        elif line.startswith("[fragment] candidate="):
            candidates += 1
        elif line.startswith("[fragment] decode_ready="):
            parts = line.split()
            for part in parts:
                if part.startswith("decode_ready="):
                    decode_ready = part.split("=", 1)[1]
                elif part.startswith("reason="):
                    decode_ready_reason = part.split("=", 1)[1]
        elif line.startswith("[fragment] codec_warning "):
            codec_warnings.append(line.split(" ", 2)[2])
        elif line.startswith("[vote] candidate="):
            voted_candidates += 1
        elif line.startswith("[vote] no groups"):
            vote_no_groups = True
        elif line.startswith("[fragment] decode="):
            if "decode=ok" in line:
                decode_ok += 1
            elif "decode=failed" in line:
                decode_failed += 1
        elif line.startswith("[fragment] ffmpeg=missing"):
            ffmpeg_missing = True
        elif line.startswith("[features] "):
            for part in line.split():
                if part.startswith("rows="):
                    features_rows = part.split("=", 1)[1]
        elif line.startswith("[live_analyze] skipped_low_image_quality"):
            low_quality = line.split(" ", 1)[1] if " " in line else "low_quality"
        elif "could not detect video PID" in line or "could not detect H.264 video PID" in line:
            error = "no_video_pid"

    if error:
        parts = [error]
        for warning in codec_warnings[:2]:
            parts.append(f"codec_warning={warning}")
        return " ".join(parts)
    if ffmpeg_missing:
        return "ffmpeg_missing"
    if decode_ready == "no" and decode_ready_reason:
        detail = decode_ready_reason
    elif decode_ready == "no":
        detail = "not_decode_ready"
    elif decode_ready == "yes" and decode_ok == 0 and decode_failed == 0:
        detail = "decode_ready_no_attempt"
    elif decode_ready == "yes":
        detail = "decode_ready"
    else:
        detail = ""
    if access_units == "0":
        return "no_access_units"
    if not detail and (key_units or idr_units) == "0":
        detail = "no_idr"
    elif not detail and candidates == 0:
        detail = "no_candidates"
    elif decode_ok == 0 and decode_failed > 0:
        detail = "decode_failed"
    elif low_quality:
        detail = "low_image_quality"
    elif not detail:
        detail = "no_decoded_png"

    parts = [detail]
    if access_units is not None:
        parts.append(f"au={access_units}")
    if codec is not None:
        parts.append(f"codec={codec}")
    if idr_units is not None:
        parts.append(f"idr={idr_units}")
    if key_units is not None:
        parts.append(f"key={key_units}")
    if vps is not None:
        parts.append(f"vps={vps}")
    if sps is not None:
        parts.append(f"sps={sps}")
    if pps is not None:
        parts.append(f"pps={pps}")
    if candidates or voted_candidates:
        parts.append(f"candidates={candidates}+{voted_candidates}")
    if decode_ok or decode_failed:
        parts.append(f"decode_ok={decode_ok}")
        parts.append(f"decode_failed={decode_failed}")
    if features_rows is not None:
        parts.append(f"features={features_rows}")
    if vote_no_groups:
        parts.append("vote_groups=0")
    for warning in codec_warnings[:2]:
        parts.append(f"codec_warning={warning}")
    return " ".join(parts)


def count_ts_packets(path: Path) -> int:
    try:
        return path.stat().st_size // 188
    except FileNotFoundError:
        return 0


def trim_capture(path: Path, max_bytes: int) -> None:
    size = path.stat().st_size
    if size <= max_bytes:
        return
    keep = max_bytes - (max_bytes % 188)
    with path.open("rb") as f:
        f.seek(size - keep)
        data = f.read()
    sync = data.find(b"\x47")
    if sync > 0:
        data = data[sync:]
        data = data[: len(data) - (len(data) % 188)]
    tmp = path.with_suffix(".tmp")
    tmp.write_bytes(data)
    tmp.replace(path)


def _section_from_packet(pkt: bytes) -> bytes | None:
    if len(pkt) != 188 or pkt[0] != 0x47:
        return None
    afc = (pkt[3] >> 4) & 0x03
    off = 4
    if afc in (2, 3):
        if off >= len(pkt):
            return None
        off += 1 + pkt[off]
    if afc not in (1, 3) or off >= 188:
        return None
    if pkt[1] & 0x40:
        pointer = pkt[off]
        off += 1 + pointer
    if off + 3 > 188:
        return None
    section_len = ((pkt[off + 1] & 0x0F) << 8) | pkt[off + 2]
    end = off + 3 + section_len
    if end > 188:
        return None
    return pkt[off:end]


def _dvb_text(data: bytes) -> str:
    if data and data[0] in (0x05, 0x10, 0x11, 0x12, 0x15):
        data = data[1:]
    return "".join(chr(c) if 0x20 <= c <= 0x7E else "?" for c in data)


def scan_service_info(path: Path) -> dict[str, object]:
    info: dict[str, object] = {}
    try:
        data = path.read_bytes()
    except FileNotFoundError:
        return info

    pmt_pids: set[int] = set()
    for pos in range(0, len(data) - 187, 188):
        pkt = data[pos:pos + 188]
        if pkt[0] != 0x47:
            continue
        pid = ((pkt[1] & 0x1F) << 8) | pkt[2]
        section = _section_from_packet(pkt)
        if not section:
            continue
        table_id = section[0]
        if pid == 0x0000 and table_id == 0x00 and len(section) >= 12:
            section_len = ((section[1] & 0x0F) << 8) | section[2]
            end = 3 + section_len - 4
            p = 8
            while p + 4 <= end:
                service_id = (section[p] << 8) | section[p + 1]
                pmt_pid = ((section[p + 2] & 0x1F) << 8) | section[p + 3]
                if service_id != 0:
                    info.setdefault("service_id", service_id)
                    info.setdefault("pmt_pid", pmt_pid)
                    pmt_pids.add(pmt_pid)
                p += 4
        elif table_id == 0x02 and (pid in pmt_pids or "pmt_pid" not in info) and len(section) >= 16:
            service_id = (section[3] << 8) | section[4]
            pcr_pid = ((section[8] & 0x1F) << 8) | section[9]
            info.setdefault("service_id", service_id)
            info["pmt_pid"] = pid
            info["pcr_pid"] = pcr_pid
            section_len = ((section[1] & 0x0F) << 8) | section[2]
            end = 3 + section_len - 4
            program_info_len = ((section[10] & 0x0F) << 8) | section[11]
            p = 12 + program_info_len
            video: list[str] = []
            audio: list[str] = []
            while p + 5 <= end:
                stream_type = section[p]
                stream_pid = ((section[p + 1] & 0x1F) << 8) | section[p + 2]
                es_info_len = ((section[p + 3] & 0x0F) << 8) | section[p + 4]
                if stream_type in (0x02, 0x1B, 0x24):
                    video.append(f"0x{stream_pid:04x}/0x{stream_type:02x}")
                elif stream_type in (0x03, 0x04, 0x0F, 0x11):
                    audio.append(f"0x{stream_pid:04x}/0x{stream_type:02x}")
                p += 5 + es_info_len
            if video:
                info["video"] = ",".join(video)
            if audio:
                info["audio"] = ",".join(audio)
        elif pid == 0x0011 and table_id in (0x42, 0x46) and len(section) >= 14:
            ts_id = (section[3] << 8) | section[4]
            original_network_id = (section[8] << 8) | section[9]
            section_len = ((section[1] & 0x0F) << 8) | section[2]
            end = 3 + section_len - 4
            p = 11
            while p + 5 <= end:
                service_id = (section[p] << 8) | section[p + 1]
                descriptors_len = ((section[p + 3] & 0x0F) << 8) | section[p + 4]
                d = p + 5
                dend = d + descriptors_len
                while d + 2 <= dend and d + 2 <= len(section):
                    tag = section[d]
                    size = section[d + 1]
                    body = section[d + 2:d + 2 + size]
                    if tag == 0x48 and len(body) >= 3:
                        provider_len = body[1]
                        name_pos = 2 + provider_len
                        if name_pos < len(body):
                            service_len = body[name_pos]
                            info["service_id"] = service_id
                            info["ts_id"] = ts_id
                            info["original_network_id"] = original_network_id
                            info["service_type"] = body[0]
                            info["provider_name"] = _dvb_text(body[2:name_pos])
                            info["service_name"] = _dvb_text(body[name_pos + 1:name_pos + 1 + service_len])
                    d += 2 + size
                p = dend
    return info


def service_info_text(info: dict[str, object]) -> str:
    parts: list[str] = []
    for key in ("service_id", "service_name", "provider_name", "pmt_pid", "pcr_pid", "video", "audio", "original_network_id"):
        if key not in info:
            continue
        value = info[key]
        if isinstance(value, int) and key.endswith("_pid"):
            parts.append(f"{key}=0x{value:04x}")
        else:
            parts.append(f"{key}={value}")
    return " ".join(parts)


def first_video_pid(info: dict[str, object]) -> int | None:
    video = info.get("video")
    if not isinstance(video, str) or not video:
        return None
    first = video.split(",", 1)[0].split("/", 1)[0]
    try:
        return int(first, 16)
    except ValueError:
        return None


def color_service_line(line: str, service_text: str) -> str:
    if sys.stderr.isatty() and ("service_name=" in service_text or "provider_name=" in service_text):
        return f"{YELLOW}{line}{RESET}"
    return line


def image_quality(path: Path) -> float:
    if path.stat().st_size < 1024:
        return 0.0
    with Image.open(path) as im:
        rgb = im.convert("RGB")
        stat = ImageStat.Stat(rgb)
    std = sum(stat.stddev) / 3.0
    means = stat.mean
    color_spread = max(means) - min(means)
    # Uniform gray decoder output has very low stddev and color spread.
    quality = min(1.0, std / 45.0) * 0.75 + min(1.0, color_spread / 80.0) * 0.25
    return max(0.0, min(1.0, quality))


def analyze(capture: Path, work_dir: Path, model: Path | None, video_pid: int | None, codec: str, max_candidates: int,
            min_image_quality: float) -> tuple[float | None, Path | None, str]:
    out_dir = work_dir / "analysis"
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    cmd = [
        str(SCRIPT_DIR / "contest_fragment_vote.py"),
        "--input",
        str(capture),
        "--output-dir",
        str(out_dir),
        "--decode",
        "--vote",
        "--max-candidates",
        str(max_candidates),
        "--features-csv",
        str(out_dir / "features.csv"),
    ]
    if model:
        cmd.extend(["--ml-model", str(model)])
    if video_pid is not None:
        cmd.extend(["--video-pid", f"0x{video_pid:04x}"])
    if codec != "auto":
        cmd.extend(["--codec", codec])

    proc = run(cmd)
    log = proc.stdout + proc.stderr
    if proc.returncode != 0:
        return None, None, log

    scores = parse_candidate_scores(log)
    skipped: list[str] = []
    for score, candidate in scores:
        png = out_dir / Path(candidate).with_suffix(".png").name
        if png.exists():
            quality = image_quality(png)
            if quality >= min_image_quality:
                if skipped:
                    log += "[live_analyze] skipped_low_image_quality " + " ".join(skipped) + "\n"
                return score, png, log + f"[live_analyze] selected={png.name} ml={score:.3f} image_quality={quality:.3f}\n"
            skipped.append(f"{png.name}:ml={score:.3f}:image_quality={quality:.3f}")
    if skipped:
        log += "[live_analyze] skipped_low_image_quality " + " ".join(skipped) + "\n"
    return None, None, log


def maybe_update_best(capture: Path, work_dir: Path, model: Path | None, max_candidates: int,
                      shared_frame: SharedFrame, log_path: Path, best_score: float | None, reason: str,
                      min_image_quality: float, video_pid: int | None, codec: str, timing: str = "") -> float | None:
    packets = count_ts_packets(capture)
    score, png, log = analyze(capture, work_dir, model, video_pid, codec, max_candidates, min_image_quality)
    log_path.write_text(log)
    if score is None or png is None:
        detail = summarize_analysis_log(log)
        print(f"[live] analyze=no_candidate reason={reason}{timing} packets={packets} detail={detail}", file=sys.stderr)
        return best_score
    if best_score is None or score > best_score:
        version = shared_frame.update_from_png(png)
        shutil.copyfile(png, work_dir / "best.png")
        print(
            f"[live] best_update reason={reason}{timing} score={score:.3f} packets={packets} frame_version={version}",
            file=sys.stderr,
        )
        return score
    print(f"[live] best_keep reason={reason}{timing} score={best_score:.3f} candidate={score:.3f} packets={packets}", file=sys.stderr)
    return best_score


def start_udp_output(shared_frame: SharedFrame, udp_output: str, fps: float, size: tuple[int, int],
                     stop_event: threading.Event) -> tuple[subprocess.Popen[bytes], threading.Thread]:
    if shutil.which("ffmpeg") is None:
        raise SystemExit("ffmpeg not found; cannot use --udp-output")

    width, height = size
    url = f"udp://{udp_output}?pkt_size=1316"
    cmd = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "warning",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgb24",
        "-s",
        f"{width}x{height}",
        "-r",
        f"{fps:g}",
        "-i",
        "-",
        "-an",
        "-c:v",
        "mpeg2video",
        "-q:v",
        "2",
        "-flags",
        "+low_delay",
        "-pix_fmt",
        "yuv420p",
        "-g",
        "1",
        "-bf",
        "0",
        "-f",
        "mpegts",
        "-mpegts_flags",
        "resend_headers",
        "-muxdelay",
        "0",
        "-muxpreload",
        "0",
        "-flush_packets",
        "1",
        url,
    ]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stderr=subprocess.PIPE)

    def stderr_loop() -> None:
        if proc.stderr is None:
            return
        for raw_line in proc.stderr:
            line = raw_line.decode("utf-8", errors="replace").strip()
            if line:
                print(f"[live_ffmpeg] {line}", file=sys.stderr)

    def output_loop() -> None:
        frame_interval = 1.0 / fps
        current_version = -1
        current, current_version = shared_frame.get()
        while not stop_event.is_set():
            returncode = proc.poll()
            if returncode is not None:
                print(f"[live] udp_output_stopped returncode={returncode}", file=sys.stderr)
                break
            frame, version = shared_frame.get()
            if version != current_version:
                current = frame
                current_version = version

            if proc.stdin is None:
                break
            try:
                proc.stdin.write(current)
                proc.stdin.flush()
            except BrokenPipeError:
                returncode = proc.poll()
                print(f"[live] udp_output_broken_pipe returncode={returncode}", file=sys.stderr)
                break
            time.sleep(frame_interval)

        if proc.stdin:
            try:
                proc.stdin.close()
            except BrokenPipeError:
                pass

    stderr_thread = threading.Thread(target=stderr_loop, name="contest-udp-stderr", daemon=True)
    stderr_thread.start()
    thread = threading.Thread(target=output_loop, name="contest-udp-output", daemon=True)
    thread.start()
    print(f"[live] udp_output={udp_output} fps={fps:g} size={width}x{height}", file=sys.stderr)
    return proc, thread


def timing_text(started_at: float, first_input_at: float | None) -> str:
    now = time.monotonic()
    parts = [f"t={now - started_at:.1f}s"]
    if first_input_at is not None:
        parts.append(f"rx={now - first_input_at:.1f}s")
    return " " + " ".join(parts)


def main() -> int:
    parser = argparse.ArgumentParser(description="Read live TS from stdin and update the best recovered contest frame.")
    parser.add_argument("--work-dir", default=Path("temp/live"), type=Path)
    parser.add_argument("--model", type=Path, help="Optional contest ML model JSON")
    parser.add_argument("--interval", type=float, default=5.0, help="Seconds between analyses")
    parser.add_argument("--startup-interval", type=float, default=0.0,
                        help="Seconds between analyses until the first best image is found")
    parser.add_argument("--analysis-packets", type=int, default=500, help="Minimum new TS packets between analyses")
    parser.add_argument("--startup-analysis-packets", type=int, default=1,
                        help="Minimum new TS packets between analyses until the first best image is found")
    parser.add_argument("--max-mb", type=float, default=8.0, help="Rolling capture size")
    parser.add_argument("--min-packets", type=int, default=1, help="Minimum TS packets before first analysis")
    parser.add_argument("--max-candidates", type=int, default=8)
    parser.add_argument("--video-pid", type=lambda s: int(s, 0),
                        help="Force the video PID, for example 0x0100")
    parser.add_argument("--codec", choices=("auto", "h264", "h265"), default="auto",
                        help="Force the elementary stream codec for fragment recovery")
    parser.add_argument("--min-image-quality", type=float, default=0.15,
                        help="Reject decoded candidates below this 0-1 visual quality threshold")
    parser.add_argument("--chunk-size", type=int, default=1316)
    parser.add_argument("--append", action="store_true", help="Append to an existing rolling.ts instead of starting fresh")
    parser.add_argument("--udp-output", help="Optional HOST:PORT MPEG-TS UDP output of the in-memory best frame")
    parser.add_argument("--output-fps", type=float, default=1.0)
    parser.add_argument("--output-size", type=parse_size, default=(640, 360))
    args = parser.parse_args()

    args.work_dir.mkdir(parents=True, exist_ok=True)
    capture = args.work_dir / "rolling.ts"
    log_path = args.work_dir / "live.log"
    max_bytes = int(args.max_mb * 1024 * 1024)
    shared_frame = SharedFrame(args.output_size)

    best_score: float | None = None
    last_analysis = 0.0
    last_analysis_packets = 0
    last_service_text = ""
    started_at = time.monotonic()
    first_input_at: float | None = None
    capture.parent.mkdir(parents=True, exist_ok=True)
    if not args.append and capture.exists():
        capture.unlink()
    if not args.append:
        if log_path.exists():
            log_path.unlink()
        analysis_dir = args.work_dir / "analysis"
        if analysis_dir.exists():
            shutil.rmtree(analysis_dir)

    stop_event = threading.Event()
    udp_proc = None
    udp_thread = None
    if args.udp_output:
        udp_proc, udp_thread = start_udp_output(shared_frame, args.udp_output, args.output_fps, args.output_size, stop_event)

    print(f"[live] work_dir={args.work_dir} capture={capture} best=in_memory", file=sys.stderr)
    try:
        with capture.open("ab", buffering=0) as out:
            while True:
                chunk = sys.stdin.buffer.read(args.chunk_size)
                if not chunk:
                    break
                if first_input_at is None:
                    first_input_at = time.monotonic()
                    print(f"[live] first_input t={first_input_at - started_at:.1f}s bytes={len(chunk)}", file=sys.stderr)
                out.write(chunk)
                now = time.monotonic()
                interval = args.startup_interval if best_score is None else args.interval
                if now - last_analysis < interval:
                    continue
                last_analysis = now
                out.flush()
                trim_capture(capture, max_bytes)
                packets = count_ts_packets(capture)
                service_info = scan_service_info(capture)
                service_text = service_info_text(service_info)
                if service_text and service_text != last_service_text:
                    service_line = f"[live] service{timing_text(started_at, first_input_at)} {service_text}"
                    print(color_service_line(service_line, service_text), file=sys.stderr)
                    last_service_text = service_text
                    service_video_pid = first_video_pid(service_info)
                    if args.video_pid is not None and service_video_pid is not None and args.video_pid != service_video_pid:
                        print(
                            f"[live] video_pid_warning{timing_text(started_at, first_input_at)} "
                            f"forced=0x{args.video_pid:04x} pmt=0x{service_video_pid:04x} hint=remove --video-pid or set --video-pid 0x{service_video_pid:04x}",
                            file=sys.stderr,
                        )
                if packets < args.min_packets:
                    print(
                        f"[live]{timing_text(started_at, first_input_at)} packets={packets} waiting_min={args.min_packets}",
                        file=sys.stderr,
                    )
                    continue
                packet_step = args.startup_analysis_packets if best_score is None else args.analysis_packets
                if packets - last_analysis_packets < packet_step:
                    continue

                best_score = maybe_update_best(
                    capture, args.work_dir, args.model, args.max_candidates, shared_frame, log_path, best_score, "interval",
                    args.min_image_quality, args.video_pid, args.codec, timing_text(started_at, first_input_at)
                )
                last_analysis_packets = packets

        if count_ts_packets(capture) >= args.min_packets:
            best_score = maybe_update_best(
                capture, args.work_dir, args.model, args.max_candidates, shared_frame, log_path, best_score, "eof",
                args.min_image_quality, args.video_pid, args.codec, timing_text(started_at, first_input_at)
            )
        print("[live] input_eof", file=sys.stderr)
    finally:
        stop_event.set()
        if udp_thread:
            udp_thread.join(timeout=2.0)
        if udp_proc:
            try:
                udp_proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                udp_proc.terminate()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
