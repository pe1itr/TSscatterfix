# TSscatterfix

TSscatterfix is an experimental MPEG Transport Stream repair and recovery layer for weak-signal DVB/DATV reception. It is intended to run as live middleware between a demodulator/decoder and a media player such as `ffplay` or VLC.

It is not a video reconstruction tool. Version 1 does not guess H.264/H.265 frames, rewrite PCR, run FEC, or repair damaged elementary stream payloads. Its job is to keep the TS structure understandable for downstream decoders when RF conditions are marginal.

## Problem

Weak-signal DATV links can produce short fades, burst errors, airplane scatter, troposcatter effects, packet loss, and temporary PSI loss. A video decoder may have valid payload nearby but still fail to lock quickly if sync, continuity, PAT, or PMT information is unstable.

TSscatterfix tries to reduce that lock penalty by preserving 188-byte alignment, tracking continuity, caching valid PSI/SI, and cautiously reinjecting known-good PAT/PMT/SDT packets.

## Approach

Classic TS repair tools often treat a stream as a damaged file. TSscatterfix is designed as realtime middleware:

```bash
decoder_command | ./tsscatterfix --verbose | ffplay -
```

The repair policy is conservative:

- pass valid TS packets unchanged;
- resync on MPEG-TS sync byte `0x47`;
- drop corrupt bytes before the next sync point;
- track continuity per PID and log uncertainty;
- parse PAT, PMT, and DVB SDT sections when CRC is valid;
- cache the most recent valid PAT/PMT/SDT packets;
- periodically reinject cached PSI to help players lock;
- infer PID roles with simple weighted confidence scores.

All logs go to `stderr`. `stdout` is reserved for binary TS output.

## What TSscatterfix Tries To Repair

Version 1 focuses on repeated MPEG-TS structure, not on reconstructing lost video or audio.

Good repair targets:

- **TS packet alignment**: recover 188-byte packet boundaries after corrupt bytes or sync loss.
- **Sync loss**: find the next `0x47` sync byte and resume aligned output.
- **PAT**: cache and reinject the last valid Program Association Table.
- **PMT**: cache and reinject the last valid Program Map Table, including stream PIDs and PCR PID.
- **SDT**: cache and reinject the last valid DVB Service Description Table.
- **Service name and provider name**: recover them from a previously valid SDT `service_descriptor`.
- **Repeated PSI/SI sections**: score repeated candidates with simple weighted confidence.
- **Repeated live fragments**: count repeated TS packets and payload fragments as candidates for future voting/combining.
- **Continuity observations**: detect missing, duplicate, out-of-order, and discontinuity-marked packets.
- **PID role confidence**: infer likely PAT, PMT, VIDEO, AUDIO, PCR, NULL, or UNKNOWN roles.

Conservative repair behavior:

- valid packets are passed unchanged;
- corrupt bytes before resync are dropped;
- cached PAT/PMT/SDT packets may be injected periodically;
- `--clean-ts` can replace packets from unknown/non-program PIDs with null packets after PAT/PMT lock;
- continuity errors are logged, but counters are not aggressively rewritten;
- service/provider names are not invented, only reused from a previously CRC-valid SDT.

## What TSscatterfix Does Not Repair

Version 1 deliberately avoids high-risk payload modification:

- no H.264/H.265 video frame reconstruction;
- no MPEG audio or AAC payload reconstruction;
- no guessing of lost video/audio packets;
- no remuxing of PES into a newly timed program stream;
- no PCR timestamp rewriting;
- no PES payload rewriting;
- no Reed-Solomon, Viterbi, LDPC, or demodulator-level FEC;
- no neural network inference;
- no decoder-specific hacks;
- no guarantee that an already damaged picture becomes visually correct.

The core rule is: repeated metadata such as PAT, PMT, and SDT can be stabilized; unique media payload should be left alone unless a later version has a defensible, measurable repair method.

## Live Repetition Model

The primary use case is live weak-signal reception. TSscatterfix receives small chunks over stdin, file replay, or UDP and builds short-term history while the stream is running.

This matters especially in DATV contest use, where stations often transmit static test cards. Even when the picture is visually static, the compressed video stream is not guaranteed to be byte-identical because PCR, PTS/DTS, encoder state, GOP structure, rate control, and codec headers can change. Therefore version 1 does not blindly combine video bytes.

Version 1 does measure repetition:

- `packet_candidates`: repeated TS packets after ignoring only the continuity counter nibble.
- `payload_candidates`: repeated payload fragments for the same PID.

These counters show how much material may be useful for later weighted voting or fragment combining. In version 1, automatic repair from repeated media payload is intentionally not enabled. PAT, PMT, and SDT are already used because they are repeated metadata with CRC validation and clear structure.

`--contest-mode` is the special mode for this situation. It keeps the same middleware I/O, but changes the timing model: output is driven by recovered image state instead of by immediate input packet timing. Version 1 implements the analysis and repetition model for this mode; the recovered-view output pipeline is planned.

## Operating Modes

TSscatterfix intentionally has only two operating modes.

Normal middleware mode:

```text
decoder TS output -> TSscatterfix -> VLC/ffplay
```

Normal mode is synchronous apart from ordinary buffering delay:

```text
input TS packet -> conservative TS repair/stabilisation -> output TS packet
```

This mode repairs or stabilises transport structure. It does not reconstruct media payload.

Contest mode:

```text
decoder TS output -> TSscatterfix --contest-mode -> VLC/ffplay
```

Contest mode is asynchronous recovered-view middleware:

```text
input TS packets -> accumulate evidence -> reconstruct best current test image -> output valid stream for VLC
```

The configuration and role are still middleware. The difference is that output no longer has to track input packet timing. TSscatterfix may keep receiving weak repeated fragments, build confidence over time, and update the outgoing stream when a usable or improved test image is available.

In version 1, `--contest-mode` enables the longer repetition model and contest logging. Full recovered-view stream generation is still planned.

## Contest Middleware Concept

Contest recovery must still fit the middleware model:

```text
decoder TS output -> TSscatterfix -> VLC/ffplay
```

The difference is that normal mode is packet-synchronous, while contest mode is state-synchronous.

Normal/live middleware:

```text
input TS packet -> conservative repair -> output TS packet
```

Contest middleware:

```text
input TS packets -> accumulate evidence -> reconstruct best current image -> output valid stream for VLC
```

In this mode the output is intentionally less tied to the immediate input packet timing. TSscatterfix may keep receiving weak repeated fragments, build confidence over time, and only update the outgoing stream when the reconstructed image improves.

The intended output is not a standalone GUI and not only a saved snapshot. It should still be something VLC/ffplay can consume, for example:

- a generated MPEG-TS stream containing the current best reconstructed still image;
- a repeated low-frame-rate video stream that updates when confidence improves;
- optionally the repaired original TS when no reconstructed image is available yet.

Important design constraints:

- stdout/UDP output must remain a valid continuous media stream;
- reset must clear contest image state without restarting the process;
- service/PID/codec/resolution changes should trigger automatic state reset;
- the input clock and output clock may be decoupled;
- logs must explain when the output image was updated and why;
- media payload guessing must stay disabled in normal mode.

Future contest controls may include:

```text
--contest-reset
--reset-on-service-change
--contest-output-rate N
--contest-min-confidence N
--no-original-ts-fallback
```

This is separate from VLC/ffplay's normal error concealment. VLC tries to play the current stream in real time; contest mode would try to build the best current picture from repeated weak-signal evidence and then feed that result forward as a valid stream.

## Contest Image Reconstruction Design

This section describes the intended design for future contest reconstruction. It is not fully implemented in version 1.

### Goal

Contest mode is for static or near-static DATV test images, especially at low symbol rates where one complete usable image may require multiple weak passes.

The goal is not to reconstruct arbitrary moving video. The goal is:

```text
receive repeated weak fragments -> accumulate evidence -> recover one best current test image -> feed VLC/ffplay a valid stream
```

### Assumptions

Contest reconstruction may assume:

- the transmitted scene is static or changes slowly;
- resolution is small, for example 360x240, 426x240, 640x360, or similar;
- GOPs/keyframes repeat often enough to provide multiple attempts;
- PAT/PMT/SDT identify the video PID and codec;
- codec headers recur or can be cached;
- audio is optional and may be ignored in reconstruction mode.

It may not assume:

- TS packets repeat byte-for-byte;
- compressed frames are identical on every pass;
- timestamps, PCR, continuity counters, or PES headers are stable;
- every keyframe is complete;
- the input bitrate is sufficient for realtime decoding.

### Pipeline

Planned contest mode pipeline:

```text
TS input
  -> sync/resync
  -> PAT/PMT/SDT lock
  -> video PID detection
  -> PES reassembly
  -> codec header detection/cache
  -> access-unit/keyframe detection
  -> fragment store
  -> multi-pass alignment
  -> confidence voting
  -> decode attempts
  -> best-image scoring
  -> async TS/video output to VLC
```

### Useful Anchors

The reconstruction layer needs anchors that are more stable than raw TS packet position.

Transport anchors:

- PID;
- payload unit start indicator;
- continuity counter sequence;
- PCR interval;
- PES start location;
- PTS/DTS when present.

Codec anchors for H.264:

- NAL start codes;
- SPS;
- PPS;
- AUD if present;
- IDR slices;
- slice headers where parseable.

Codec anchors for H.265/HEVC:

- NAL start codes;
- VPS;
- SPS;
- PPS;
- AUD if present;
- IDR/CRA/BLA access points;
- slice headers where parseable.

### Fragment Store

Instead of treating each TS packet as final, contest mode should store fragments with context:

```text
codec
service id
video pid
access-unit id or candidate id
nal type
fragment offset
fragment length
payload bytes
source packet index
continuity quality
tei flag
confidence
```

Fragments with TEI, continuity gaps, or uncertain offset receive lower confidence. Repeated matching fragments receive higher confidence.

### Multi-Pass Combining

At very low symbol rates, one pass may only provide part of a keyframe. A later pass may provide a different part. Contest async mode should combine passes only when context matches.

Possible matching keys:

- same service and video PID;
- same codec and resolution;
- same SPS/PPS/VPS identity;
- same NAL type;
- same keyframe/access-unit pattern;
- compatible fragment offset;
- repeated payload hash or near-match;
- plausible continuity/PES structure.

Voting levels, from safest to riskiest:

1. Exact repeated PSI/SI sections with CRC.
2. Exact repeated codec headers such as SPS/PPS/VPS.
3. Exact repeated NAL fragments at the same normalized offset.
4. Majority byte voting inside repeated fragments.
5. Decoder-assisted candidate selection.

Version 1 only implements the first category for active repair and measures repeated packets/payloads for later work.

### Decode Attempts

Contest async mode should not need to implement a full H.264/H.265 decoder itself. A practical implementation can use libavcodec or an optional helper process later.

Candidate decode process:

```text
cached codec headers + reconstructed keyframe candidate -> decoder -> frame or error report
```

A decoded frame can be scored by:

- decoder success/failure;
- visible resolution;
- number of decoded macroblocks/CTUs if available;
- absence of severe decoder errors;
- stability compared with previous best frame;
- amount of high-confidence source data;
- whether SPS/PPS/VPS and IDR were complete.

### Output To VLC

The output should remain a valid VLC/ffplay-consumeable stream.

Possible output strategy:

```text
generate a low-frame-rate MPEG-TS stream
repeat the current best reconstructed image
update the image when a better candidate appears
keep PAT/PMT/SDT/PCR valid
```

The output clock may be independent from the input clock. If no reconstructed image is available yet, options include:

- pass through repaired original TS;
- output a black/placeholder frame;
- output no media until first confident image;
- keep repeating the last best image.

### Reset Behavior

Contest async mode needs explicit and automatic reset paths.

Manual reset should clear:

- fragment store;
- cached access units;
- best-image candidate;
- confidence history;
- decoder error history.

Automatic reset should happen on:

- service id change;
- video PID change;
- codec change;
- resolution change;
- incompatible SPS/PPS/VPS change;
- long loss of lock;
- user command.

Possible future control options:

```text
--contest-reset
--reset-on-service-change
--contest-output-rate N
--contest-min-confidence N
--contest-state PATH
```

### Safety Rules

- Normal mode must never perform media payload reconstruction.
- Contest reconstruction must be opt-in through `--contest-mode`.
- Every reconstructed output update must have a logged reason and confidence.
- Low-confidence fragments must not overwrite high-confidence fragments.
- The original TS repair path and contest image reconstruction path must remain separable.
- If reconstruction fails, TSscatterfix should degrade gracefully to pass-through or last-best output.

## Build

```bash
make
```

The implementation is C11, uses only standard/POSIX C library calls, and has no heavy external dependencies.

On Windows, the UDP support uses Winsock. With MinGW, link Winsock explicitly:

```bash
make LDLIBS=-lws2_32
```

## Usage

Pipe mode:

```bash
decoder_command | ./tsscatterfix | ffplay -
```

Verbose pipe mode:

```bash
decoder_command | ./tsscatterfix --verbose | ffplay -
```

File input/output:

```bash
./tsscatterfix --input broken.ts --output fixed.ts --verbose
```

Clean TS file output:

```bash
./tsscatterfix --input broken.ts --output clean.ts --clean-ts
```

UDP input to stdout:

```bash
./tsscatterfix --udp-input 1234 | ffplay -
```

File input to UDP output:

```bash
./tsscatterfix --input broken.ts --udp-output 127.0.0.1:1234
```

File replay to UDP with pacing:

```bash
./tsscatterfix --udp-output 127.0.0.1:1234 --udp-rate-kbit 350 < broken.ts
```

Looped file replay to UDP, useful when starting VLC manually:

```bash
./tsscatterfix --input broken.ts --loop-input --udp-output 127.0.0.1:1234
```

Clean TS replay to UDP:

```bash
./tsscatterfix --input broken.ts --loop-input --clean-ts --udp-output 127.0.0.1:1234
```

UDP middleware:

```bash
./tsscatterfix --udp-input 1234 --udp-output 127.0.0.1:1235 --verbose
```

Contest/static test-card middleware:

```bash
./tsscatterfix --udp-input 1234 --udp-output 127.0.0.1:1235 --mode contest --verbose
```

Prototype still-image contest recovery from a capture:

```bash
tools/contest_recover.py --input example/linrad_20260516_220739_150k.ts --output /tmp/linrad_recovered.png
```

Probe whether a capture has enough H.264 evidence for contest recovery:

```bash
tools/contest_probe.py --input example/linrad_20260516_220739_150k.ts
```

Build H.264 IDR fragment candidates and decode them to PNGs:

```bash
tools/contest_fragment_vote.py --input example/linrad_20260516_220739_150k.ts --output-dir /tmp/linrad_fragments --decode
```

Multiple captures can be ranked together by repeating `--input`:

```bash
tools/contest_fragment_vote.py --input example/linrad_20260516_220739_150k.ts --input example/linrad_20260516_221341_150k.ts --output-dir /tmp/linrad_fragments_combined --decode
```

Add `--vote` to also create byte-voted `.h264` candidates from similarly sized
IDR slices:

```bash
tools/contest_fragment_vote.py --input example/linrad_20260516_220739_150k.ts --input example/linrad_20260516_221341_150k.ts --output-dir /tmp/linrad_fragments_voted --decode --vote
```

Export confidence features for later ML experiments:

```bash
tools/contest_fragment_vote.py --input example/linrad_20260516_220739_150k.ts --output-dir /tmp/linrad_features --features-csv /tmp/linrad_features/features.csv
```

Create a label template, train a small model, then use it for ranking:

```bash
tools/contest_label_candidates.py --features-csv /tmp/linrad_features/features.csv --candidate-dir /tmp/linrad_features --output /tmp/linrad_features/labels.csv --idr-only
tools/contest_autolabel.py --labels-csv /tmp/linrad_features/labels.csv --candidate-dir /tmp/linrad_features --output /tmp/linrad_features/labels_auto.csv --overwrite
tools/contest_train.py --features-csv /tmp/linrad_features/features.csv --labels-csv /tmp/linrad_features/labels.csv --output-model /tmp/linrad_features/model.json
tools/contest_fragment_vote.py --input example/linrad_20260516_220739_150k.ts --output-dir /tmp/linrad_ml_ranked --ml-model /tmp/linrad_features/model.json --decode --vote
```

Multiple labelled datasets can be trained together by repeating matching
`--features-csv` and `--labels-csv` arguments:

```bash
tools/contest_train.py \
  --features-csv temp/linrad_fragments_5way_conf_csv/features.csv \
  --labels-csv temp/linrad_fragments_5way_conf/labels_auto.csv \
  --features-csv temp/scatterfix_live_test/features.csv \
  --labels-csv temp/scatterfix_live_test/labels.csv \
  --output-model temp/linrad_fragments_5way_conf/model_combined.json
```

Live stdin prototype that keeps the best recovered frame in memory:

```bash
decoder_command \
| tools/contest_live.py \
    --work-dir temp/live \
    --model temp/linrad_fragments_5way_conf/model_combined.json
```

Live stdin prototype with MPEG-TS UDP output for VLC:

```bash
decoder_command \
| tools/contest_live.py \
    --work-dir temp/live \
    --model temp/linrad_fragments_5way_conf/model_combined.json \
    --udp-output 127.0.0.1:1235 \
    --output-fps 1 \
    --output-size 640x360
```

Open VLC/ffplay on `udp://@:1235`.

This uses TSscatterfix for TS cleanup, ffmpeg for H.264 decoding, and then combines
decoded frames into one PNG plus a confidence mask. It only preserves observed
image evidence; unseen parts of a callsign are not invented.

VLC example:

```bash
decoder_command | ./tsscatterfix --psi-interval-ms 500 | vlc -
```

Dry-run analysis:

```bash
./tsscatterfix --input broken.ts --dry-run --verbose
```

## Example Captures

Example material can be placed in `example/` or `examples/`.

Original `.ts` captures are the most useful test inputs because they still contain MPEG-TS packet alignment, sync bytes, continuity counters, PAT, PMT, SDT, PCR, null packets, and transport-level damage.

Repaired `.mp4` files are still useful as visual reference material, but they are not suitable for validating TSscatterfix repair behavior. During MP4 remuxing, the original 188-byte TS packet structure and DVB PSI/SI metadata are normally discarded.

## Options

```text
--input PATH
--output PATH
--udp-input PORT
--udp-output HOST:PORT
--udp-rate-kbit N
--loop-input
--psi-interval-ms N
--no-psi-inject
--passthrough
--sanitize
--clean-ts
--drop-audio
--smooth-video-cc
--smooth-timestamps
--stats-interval N
--max-packets N
--mode normal|contest
--normal-mode
--contest-mode
--verbose
--dry-run
--no-ml
--ml-state PATH
--help
```

`--psi-interval-ms` controls periodic PAT/PMT reinjection from the valid PSI cache. The default is 500 ms.

`--no-psi-inject` disables periodic cached PAT/PMT/SDT injection. TSscatterfix also disables PSI injection automatically for file/stdin replay to UDP unless `--psi-interval-ms` is explicitly set, so replay behaves closer to pass-through.

`--passthrough` disables packet modification, including PSI injection and TEI cache replacement. It is useful for checking whether UDP/file I/O itself works before enabling repair behaviour. `--no-repair` is accepted as an alias.

`--sanitize` replaces unrepairable corrupt packets and unrepairable TEI packets with null packets. This preserves TS packet cadence while preventing known-bad payload from reaching VLC/ffplay.

`--clean-ts` is an experimental stricter packet-level cleanup profile. It implies `--sanitize`. After a valid PAT/PMT has identified the program, TSscatterfix preserves PAT, PMT, PCR, video, audio, and null packets, then replaces other PIDs with null packets. This is meant for damaged captures where corrupted SI/private/unknown PIDs confuse VLC/ffplay. It does not decode, remux, repair H.264/H.265 slices, or rewrite PCR; it only suppresses packets that are outside the known selected program. PSI injection is disabled by default in this mode unless `--psi-interval-ms` is explicitly set, so packet cadence stays stable.

In `--clean-ts`, bad or unparseable PAT/PMT/SDT packets are replaced from the last valid cache when possible. For regenerated PSI/SI packets only, TSscatterfix may rewrite the continuity counter so VLC receives a coherent metadata stream. This continuity rewrite is not applied to video or audio payload packets.

`--drop-audio` is a diagnostic cleanup option intended for use with `--clean-ts`. It replaces audio PID packets with null packets while preserving the video/PCR stream. This can help distinguish TS/PSI lock problems from AAC/audio timing problems in marginal captures.

`--smooth-video-cc` is an experimental diagnostic option for `--clean-ts`. It rewrites video PID continuity counters monotonically after cleanup. This can help test whether VLC is stopping because the demuxer rejects continuity gaps, but it may hide real missing video payload from the decoder. It is not enabled by default.

`--smooth-timestamps` is an experimental diagnostic option for `--clean-ts`. It repairs large video PES PTS and PCR outliers using a conservative monotonic 25 fps estimate. This is intentionally opt-in because it modifies timing fields and can be wrong for other frame rates or variable-rate sources.

`--ml-state PATH` enables minimal persistent state storage for PID role confidence. Version 1 keeps this intentionally simple; the format is text and may change.

`--udp-input PORT` receives raw MPEG-TS over UDP. Datagrams may contain one or more 188-byte TS packets; TSscatterfix treats them as a byte stream and still performs normal sync/resync.

`--udp-output HOST:PORT` sends repaired/aligned TS packets over UDP. TSscatterfix groups 7 TS packets per UDP datagram, producing the common 1316-byte MPEG-TS-over-UDP payload size.

`--udp-rate-kbit N` paces UDP output. This is useful when replaying a file or redirected stdin to VLC; without pacing the whole file can be sent faster than VLC can lock or buffer it. Live decoder input is usually already paced and normally does not need this option.

When UDP output is used with `--input PATH` or shell redirection from a regular file, TSscatterfix automatically applies a conservative replay rate of 350 kbit/s if `--udp-rate-kbit` is not specified.

`--loop-input` loops regular file input. This is useful for UDP replay tests because VLC/ffplay may miss the beginning of a short or damaged stream if the sender is started first.

`--max-packets N` stops after N input packets. This is mainly useful for short captures and automated tests.

`--mode normal` selects normal synchronous TS repair and is the default. `--normal-mode` is accepted as an explicit alias.

`--mode contest` selects asynchronous recovered-view middleware for static DATV test images. `--contest-mode` is accepted as an alias. Version 1 uses a longer repeat-history window and reports contest-mode status in logs. Full recovered-image stream generation is planned; normal mode remains synchronous TS repair.

`tools/contest_recover.py` is an offline prototype for contest captures. It requires
`ffmpeg` and Python Pillow. It is useful for evaluating whether multiple damaged
decoded frames contain enough visual evidence to recover one still test-card image
before that behaviour is moved into live middleware.

`tools/contest_probe.py` is a lower-level recovery probe. It parses PAT/PMT,
detects the video PID, counts video continuity gaps, reassembles PES payload, and
reports H.264 NAL evidence such as SPS, PPS, IDR slices, and ordinary slices. Use
it before image recovery to decide whether a capture is missing codec headers,
missing keyframes, or mainly missing fragments that may be recoverable from more
passes.

`tools/contest_fragment_vote.py` is a first fragment-level experiment. It accepts
one or more `--input` captures, extracts H.264 access units from the video PID,
keeps observed SPS/PPS headers, ranks IDR access units by available slice data and
continuity quality, and writes candidate `.h264` elementary streams. With
`--decode`, ffmpeg also writes PNGs for visual inspection. With `--vote`, it also
groups similarly sized IDR slices and writes byte-voted candidate streams. This is
intentionally still a conservative candidate generator, not a full H.264 repair
engine.

The ranking includes deterministic TS confidence features: continuity behaviour,
scrambling bits, adaptation-field validity, payload entropy, and logical PES
starts. Candidate logs include `conf=...` and feature counters. `--features-csv`
writes those access-unit features to CSV so a later ML model can learn confidence
weights without changing the recovery pipeline.

`tools/contest_label_candidates.py` creates a `labels.csv` template for decoded
candidates. Fill `label` on a 0-5 scale, where 0 is unusable and 5 is the best
contest-readable image. `tools/contest_autolabel.py` can bootstrap approximate
labels from PNG image features such as blue-card area, white callsign energy, and
black/gray decode damage; treat these as starting labels to review, not ground
truth. `tools/contest_train.py` trains a small dependency-free linear JSON model
from one or more `features.csv` plus labels pairs. Passing `--ml-model model.json`
to `contest_fragment_vote.py` uses that model for candidate ranking and vote
weighting.

`tools/contest_live.py` is the first streaming prototype. It reads MPEG-TS bytes
from stdin into a rolling capture, periodically runs the same fragment/ML ranking
path, and keeps the current best decoded frame in process memory. With
`--udp-output HOST:PORT`, it starts ffmpeg as an output helper, feeds it the
current best image as raw RGB frames over stdin, and ffmpeg encodes/muxes a
continuous MPEG-TS UDP stream for VLC/ffplay. Each fresh run starts with a blank
black in-memory frame; pass `--append` to keep the previous rolling capture.

## Implemented In Version 1

- 188-byte MPEG-TS packet reading.
- stdin/stdout, file, and UDP input/output.
- Contest mode for asynchronous recovered-view static DATV test-card streams.
- Sync byte validation and resync.
- Dropping corrupt bytes before sync.
- Per-PID continuity counter tracking.
- Duplicate, missing, out-of-order, and discontinuity indicator logging.
- PAT parsing on PID `0x0000`.
- PMT PID discovery from PAT.
- PMT parsing for stream types, video PIDs, audio PIDs, and PCR PID.
- PAT/PMT/SDT cache.
- Periodic PSI/SI injection.
- Experimental `--clean-ts` mode for nulling unknown/non-program PIDs after PAT/PMT lock.
- Service name and provider name extraction from DVB SDT service descriptors.
- Simple PAT/PMT weighted voting.
- PID role confidence for PAT, PMT, VIDEO, AUDIO, PCR, NULL, UNKNOWN.
- PCR/PES interval tracking and large-gap logging.
- Periodic statistics to `stderr`.
- Repairability counters for invalid TS packets, TEI packets, PSI/SI cache replacements, and unrepairable TEI packets.
- Repetition counters for repeated packets and payload fragments.

## Not Implemented In Version 1

- Neural networks.
- TensorFlow, ONNX, or similar runtimes.
- Reed-Solomon, Viterbi, LDPC, or demodulator-level decoding.
- H.264/H.265 frame reconstruction.
- PCR rewriting.
- Decoder-specific hacks.
- GUI.

## RDS-AI-Decoder Inspiration

The directory `other_parties/RDS-AI-Decoder/` is reserved for placing the existing RDS-AI-Decoder software as a concept reference only.

Do not copy code from that project into TSscatterfix without explicit license review. TSscatterfix remains standalone and does not take plugin dependencies from RDS/FMDX/TEF tooling. The relevant ideas are weighted voting, confidence tracking, historical memory, context-aware reconstruction, and explicit uncertainty per decision.

## Experimental Warning

TSscatterfix version 1 is experimental. It is designed to avoid making corruption worse, but every repair decision is still heuristic. For critical workflows, keep original captures and compare decoder behavior before trusting repaired output.
