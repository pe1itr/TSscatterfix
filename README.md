# TSscatterfix

TSscatterfix is an experimental MPEG Transport Stream repair and recovery layer
for weak-signal DVB/DATV reception. It is intended to sit between a demodulator
and a media player such as `ffplay` or VLC.

The core C program is conservative middleware: it preserves TS packet alignment,
tracks continuity, caches valid PSI/SI, and reinjects known-good metadata. It
does not rewrite media payloads or try to make damaged video slices correct.

The `tools/` directory contains experimental contest/test-card recovery
prototypes. Those tools can extract H.264/H.265 keyframe candidates from repeated
weak fragments, decode them with ffmpeg, keep the best still frame, and optionally
publish that recovered still as a low-rate MPEG-TS UDP stream.

## What It Repairs

TSscatterfix is strongest at repeated transport metadata and packet structure:

- MPEG-TS 188-byte packet alignment and resync after corrupt bytes.
- PAT, PMT, and DVB SDT parsing when sections are valid.
- PAT/PMT/SDT cache and optional reinjection.
- Service name and provider name extraction from SDT service descriptors.
- Per-PID continuity tracking with duplicate, missing, and out-of-order logging.
- PID role confidence for PAT, PMT, VIDEO, AUDIO, PCR, NULL, and UNKNOWN.
- Optional cleanup modes for replacing untrusted packets with null packets.

It deliberately avoids high-risk live payload repair:

- no FEC, Viterbi, LDPC, or demodulator-level work;
- no PCR timestamp rewriting;
- no PES remuxing;
- no general H.264/H.265 reconstruction for moving video;
- no invented pixels or callsign characters.

The contest tools are more ambitious, but still conservative: they only decode
observed codec evidence and choose the best successfully decoded still frame.

## Build

```bash
make
```

The C code is C11 and uses only standard/POSIX libraries. On Windows with MinGW,
link Winsock explicitly:

```bash
make LDLIBS=-lws2_32
```

## Basic Usage

Pipe mode:

```bash
decoder_command | ./tsscatterfix | ffplay -
```

Verbose pipe mode:

```bash
decoder_command | ./tsscatterfix --verbose | ffplay -
```

File repair:

```bash
./tsscatterfix --input broken.ts --output fixed.ts --verbose
```

UDP input:

```bash
./tsscatterfix --udp-input 1234 | ffplay -
```

UDP output:

```bash
./tsscatterfix --input broken.ts --udp-output 127.0.0.1:1234
```

Paced UDP replay, useful for VLC/ffplay lock tests:

```bash
./tsscatterfix --udp-output 127.0.0.1:1234 --udp-rate-kbit 350 < broken.ts
```

Looped UDP replay:

```bash
./tsscatterfix --input broken.ts --loop-input --udp-output 127.0.0.1:1234
```

Clean TS replay:

```bash
./tsscatterfix --input broken.ts --loop-input --clean-ts --udp-output 127.0.0.1:1234
```

## Main Options

`--verbose` prints packet, PSI, continuity, and status logs to `stderr`.

`--dry-run` parses and logs without writing repaired TS output.

`--sanitize` replaces unrepairable corrupt packets and TEI packets with null
packets while preserving TS cadence.

`--clean-ts` implies `--sanitize`. After PAT/PMT lock it preserves PAT, PMT, PCR,
video, audio, SDT, and null packets, and replaces other packets with null
packets. This can help players avoid confusing damaged SI/private PIDs. It does
not decode, remux, rewrite PCR, or repair codec slices.

`--psi-interval-ms N` periodically reinjects cached PSI/SI. `0` disables periodic
injection. In `--clean-ts`, PSI injection is disabled unless this option is set.

`--udp-rate-kbit N` paces UDP output. Live demodulator input is usually already
paced; file replay often benefits from explicit pacing.

`--loop-input` loops regular file input for replay tests.

`--mode normal|contest` selects the runtime mode. `--contest-mode` is accepted as
an alias for `--mode contest`.

`--json-status` prints single-line JSON status records to `stderr`.

`--json-status-output PATH` writes those JSON status records to a file.

## Contest And Test-Card Recovery

Weak-signal DATV contest reception often carries a static or slowly changing test
card. Even when the picture is static, compressed video bytes are not guaranteed
to repeat exactly: timestamps, GOP structure, rate control, headers, and encoder
state can change. The contest tools therefore do not blindly merge arbitrary TS
packets. They look for codec anchors, cache headers, rank keyframe candidates,
and let ffmpeg decide whether a candidate can actually decode.

The Python recovery tools require `ffmpeg` and Pillow.

Supported experimental codec anchors:

- H.264/AVC: SPS, PPS, AUD, IDR.
- H.265/HEVC: VPS, SPS, PPS, AUD, IDR/CRA.

The useful receive-side milestone is a log line showing codec headers plus at
least one keyframe:

```text
codec=h265 ... key_units=1 vps=yes sps=yes pps=yes
```

or for H.264:

```text
codec=h264 ... key_units=1 sps=yes pps=yes
```

Until that happens, the tools may have video PID packets and access units, but no
decoder start point.

## Fragment Candidate Tool

`tools/contest_fragment_vote.py` extracts H.264/H.265 keyframe candidates from
one or more TS captures.

Basic decode:

```bash
tools/contest_fragment_vote.py \
  --input capture.ts \
  --output-dir /tmp/fragments \
  --decode
```

Force a known HEVC video PID:

```bash
tools/contest_fragment_vote.py \
  --input capture.ts \
  --output-dir /tmp/hevc_fragments \
  --video-pid 0x0100 \
  --codec h265 \
  --decode \
  --vote
```

Combine multiple captures:

```bash
tools/contest_fragment_vote.py \
  --input pass1.ts \
  --input pass2.ts \
  --output-dir /tmp/fragments_combined \
  --decode \
  --vote
```

`--vote` creates byte-voted `.h264` or `.h265` candidates from similarly sized
keyframe slices. This is still a conservative experiment, not a full codec repair
engine.

Export ranking features for labelling/training:

```bash
tools/contest_fragment_vote.py \
  --input capture.ts \
  --output-dir /tmp/features \
  --features-csv /tmp/features/features.csv
```

The feature set includes continuity behaviour, scrambling bits, adaptation-field
validity, payload entropy, PES starts, slice bytes, and keyframe/header evidence.

## Live Still-Frame Prototype

`tools/contest_live.py` reads live TS from stdin into a rolling capture,
periodically runs the same candidate ranking path, keeps the current best decoded
frame in memory, writes accepted best frames to `temp/live/best.png`, and can
publish the recovered still frame as a continuous low-rate MPEG-TS UDP stream.

Generic live use:

```bash
decoder_command \
| tools/contest_live.py \
    --work-dir temp/live \
    --model temp/linrad_fragments_5way_conf/model_combined.json \
    --udp-output 127.0.0.1:1235 \
    --output-fps 1 \
    --output-size 640x360
```

Live use with a known video PID but automatic codec selection:

```bash
decoder_command \
| tools/contest_live.py \
    --work-dir temp/live \
    --model temp/linrad_fragments_5way_conf/model_combined.json \
    --udp-output 127.0.0.1:1235 \
    --output-fps 1 \
    --output-size 640x360 \
    --video-pid 0x0100 \
    --codec auto
```

`--codec auto` starts with the H.264 parser and switches when PMT advertises a
supported video stream type, for example `0x24` for H.265/HEVC. Use an explicit
`--codec h264` or `--codec h265` only when PMT is missing or known to be
unreliable.

Open a player in another terminal:

```bash
ffplay -fflags nobuffer -flags low_delay -f mpegts udp://@:1235
```

or:

```bash
vlc 'udp://@:1235' --demux=ts --network-caching=300 --avcodec-hw=none
```

Live service metadata is logged when PAT/PMT/SDT are seen:

```text
[live] service ... service_id=1 service_name=PE1ORG provider_name=0201 pmt_pid=0x1000 pcr_pid=0x0100 video=0x0100/0x24 audio=0x0101/0x0f
```

Recovery status examples:

```text
[live] analyze=no_candidate ... detail=no_idr au=120 codec=h265 key=0 vps=yes sps=yes pps=yes
[live] analyze=no_candidate ... detail=no_video_pid codec_warning=pid=0x0100 stream_type=0x24 pmt_codec=h265 ignored_by_selected_codec=h264 hint=--codec h265
[live] best_update ... score=2.313 packets=777 frame_version=1
[live] best_keep ... score=2.313 candidate=2.313 packets=931
```

`codec_warning` means PMT advertised a video codec that does not match the
selected analyzer codec, or advertised a known video codec that is not supported
by the fragment tool.

`best_update` means a decoded still frame was accepted and is being repeated on
the output stream. `best_keep` means a later candidate did not improve the
current best.

## Other Tools

`tools/contest_probe.py` is a lower-level H.264 probe for checking whether a
capture has SPS/PPS/IDR evidence.

`tools/contest_recover.py` is an older offline still-image prototype for decoded
frame experiments.

`tools/contest_label_candidates.py`, `tools/contest_autolabel.py`, and
`tools/contest_train.py` support labelling decoded candidates and training a
small JSON linear model for candidate ranking.

## Implemented

- 188-byte MPEG-TS packet reading.
- stdin/stdout, file, and UDP input/output.
- Sync validation and resync.
- PAT, PMT, and SDT parsing/cache.
- Optional PSI/SI reinjection.
- Service/provider extraction from SDT.
- Per-PID continuity tracking.
- PID role confidence and JSON status output.
- Optional sanitizing and clean TS filtering.
- Contest-mode fragment observation in the C middleware.
- Experimental H.264/H.265 keyframe extraction and still-frame decoding in
  `tools/`.
- Experimental live recovered-frame UDP output in `tools/contest_live.py`.

## Not Implemented

- Neural networks or heavyweight ML runtimes.
- Reed-Solomon, Viterbi, LDPC, or demodulator-level decoding.
- General video reconstruction for arbitrary moving H.264/H.265 streams.
- PCR rewriting.
- Decoder-specific media-player hacks.
- GUI.

## Experimental Warning

TSscatterfix is experimental. Keep original captures and compare player behavior
before trusting repaired or recovered output. The safest path remains preserving
valid TS structure and valid repeated metadata; media payload recovery is limited
to the explicit contest/test-card tools.
