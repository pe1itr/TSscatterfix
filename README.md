# TSscatterfix

TSscatterfix is a C11 MPEG Transport Stream repair and still-image recovery
library for weak-signal DVB/DATV reception. The standalone programs and library
run entirely in C. No Python, Pillow, shell commands or external `ffmpeg` process
are used. Image decoding/encoding calls the FFmpeg C libraries directly.

Two interfaces serve different uses:

- `src/scatterfix.h`: repair TS data and return TS packets through a callback.
- `src/recovery.h`: reconstruct a contest/test-card image and return RGB pixels,
  service/provider names and recovery progress through callbacks. An embedded
  application can display these pixels directly in its own small window.

UDP MPEG-TS output remains available for standalone use. It is optional and is
not involved when an application displays the RGB image itself.

## Build

Install a C11 compiler, Make, pkg-config and the development packages for
`libavcodec`, `libavformat`, `libavutil`, and `libswscale`. The standalone live
output also needs C11 threads. The full build is tested on Linux.

```sh
make
make test
```

This builds `libscatterfix.a`, `tsscatterfix`, `scatterfix-contest` and
`scatterfix-model`. For TS repair without the image recovery dependencies:

```sh
make core                    # libscatterfix-core.a; no FFmpeg required
```

CMake is also supported:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The tests include actual H.264 and H.265 captures, metadata, progress, arbitrary
input chunk boundaries, reset/error handling, memory limits, PNG round trips,
and MPEG-TS image encoding.

## Embed image recovery in an application

Use CMake to compile the sources with the host program:

```cmake
add_subdirectory(path/to/TSscatterfix)
target_link_libraries(my_application PRIVATE scatterfix::scatterfix)
```

Or link the Make-built archive:

```sh
cc -Isrc my_application.c libscatterfix.a \
  $(pkg-config --libs libavcodec libavformat libavutil libswscale) -lm \
  -o my_application
```

Public entry points have C++ linkage guards. The core library creates no window,
thread, socket, temporary file or child process. The host owns the input and GUI.
A minimal integration is:

```c
#include "recovery.h"

static void on_image(void *app, const SfImage *image)
{
    /* RGB24, top row first, image->stride bytes per row.
       Copy the pixels into a GUI image/texture or a queued GUI event.
       Do not retain image->pixels after this callback returns. */
    my_window_copy_rgb(app, image->pixels, image->width,
                       image->height, image->stride);
}

static void on_status(void *app, const SfRecoveryStatus *status)
{
    my_window_set_service(app, status->service_name, status->provider_name);
    my_window_set_progress(app, status->progress_percent, status->phase);
}

SfRecoveryConfig config;
sf_recovery_config_init(&config);
config.image = on_image;
config.status = on_status;
config.userdata = my_window;
/* Optional if PAT/PMT cannot be received:
   config.video_pid = 0x100;
   config.codec = SF_CODEC_H265; */
SfRecovery *recovery = sf_recovery_create(&config);
if (!recovery) { /* handle allocation/configuration failure */ }

/* Feed TS data as it arrives; any chunk size is supported. */
if (!sf_recovery_feed(recovery, bytes, byte_count)) { /* handle error */ }
/* On a timer or batch boundary, also try an unfinished PES. This helps when
   reception stops in the middle of a frame, before the next PES start. */
if (!sf_recovery_analyze(recovery)) { /* handle error */ }

/* End of input: consume the final PES and try remaining candidates. */
if (!sf_recovery_finish(recovery)) { /* handle error */ }
sf_recovery_destroy(recovery);
```

`my_window_*` calls above represent the host application's GUI functions, not
functions provided by Scatterfix. A complete, buildable, headless example is
[`examples/embedded.c`](examples/embedded.c):

```sh
make examples/embedded
./examples/embedded captures/on4vvv_2026-06-14_181433_70cm_rolling.ts
```

Callbacks execute synchronously on the thread calling `feed`, `analyze` or
`finish`. Decoding can take time: run recovery on a worker thread and post copied
images/status to the GUI thread. Each recovery context is independent; serialize
calls to the same context and do not reenter it from a callback. Callback objects
are borrowed until return. `sf_recovery_get_status` copies a current snapshot.

`sf_recovery_reset` clears learned headers, candidate history, metadata, counters
and progress. Call it when changing station or frequency. Destroy frees all
library-owned data; it does not implicitly finish processing. Functions returning
`int` return 1 for success and 0 for failure unless otherwise documented. A
processing/callback failure is sticky until reset. Do not retry the same bytes.

The default compressed-data budget is 8 MiB, split between the current PES and
retained candidates (at most 256). Oversized PES units are discarded until the
next start. Header caches (at most 3 × 64 KiB), temporary analysis buffers and
FFmpeg's decoded-frame storage are additional. Decoder dimensions are capped at
8192 × 8192. Old candidates are evicted as the budget fills.

## Metadata and progress

`SfRecoveryStatus` exposes `service_name`, `provider_name`, `service_id`, video
PID, codec, bytes/packets received, completed access units, keyframes, available
headers, decode attempts, decoded images, accepted frame version, image quality,
retained bytes and oversized/discarded input counts.

Names are initially empty and become available when valid SDT metadata arrives.
The current PSI parser handles sections fitting within one TS packet; large,
multi-packet PSI sections are not reassembled. Force PID/codec when needed.

Progress is an **evidence-based estimate**, not a percentage of required bytes
or a time estimate. Weak reception may supply unlimited packets without the
missing headers or keyframe. The application should show the phase alongside
the percentage, and may show a separate waiting/receiving indicator.

| Phase | Percentage | Meaning |
| --- | ---: | --- |
| `SF_WAIT_SERVICE` | 0 | Waiting for a supported video PID |
| `SF_WAIT_HEADERS` | 20–59 | Video PID known; collecting codec headers |
| `SF_WAIT_KEYFRAME` | 60 | H.264 SPS/PPS or H.265 VPS/SPS/PPS present |
| `SF_READY_TO_DECODE` | 80 | Headers and a retained keyframe available |
| `SF_WAIT_USABLE_IMAGE` | 90 | Decode attempted; still waiting for acceptable image quality |
| `SF_IMAGE_READY` | 100 | A usable image has been accepted and delivered |

100% means an image is available, not that it is complete, undamaged or the best
possible image. Better candidates can still produce new image callbacks.
Progress can fall after reset, codec/PID changes or eviction of old candidates.

## Standalone TS repair

```sh
decoder_command | ./tsscatterfix | ffplay -
./tsscatterfix --input broken.ts --output fixed.ts --verbose
./tsscatterfix --udp-input 1234 --udp-output 127.0.0.1:1235
./tsscatterfix --input broken.ts --loop-input --clean-ts \
  --udp-output 127.0.0.1:1234 --udp-rate-kbit 350
```

Normal mode preserves TS packet cadence, aligns 188-byte packets, tracks
continuity, caches valid PAT/PMT/SDT and optionally reinjects metadata. `--sanitize`
replaces unrepairable packets with null packets; `--clean-ts` also filters unknown
PIDs after PMT lock. `--no-psi-inject` disables periodic injection. Optional
`--drop-audio`, `--smooth-video-cc` and `--smooth-timestamps` retain their existing
behaviour. See `./tsscatterfix --help` for all normal-mode options.

For embedded TS repair, initialize `ScatterfixConfig`, call `scatterfix_create`
with a packet callback and feed byte chunks with `scatterfix_feed`. Use
`scatterfix_stats` and `scatterfix_service` for statistics/metadata. This API is
separate from image recovery; there is no need to repair or retransmit TS before
using the image interface. `scatterfix_reset` retains cumulative TS statistics.

## Standalone contest recovery

Offline recovery:

```sh
./scatterfix-contest recover --input capture.ts --output-dir /tmp/recovered \
  --decode --vote --features-csv /tmp/recovered/features.csv
```

Add more `--input` arguments to combine captures of the **same service and
codec**. Without an output directory, results are only reported through status.
`probe` or `--no-decode` inspects codec evidence without decoding. Use
`--video-pid 0x100 --codec h265` if metadata is unavailable.

Live recovery with optional standalone UDP publication:

```sh
decoder_command | ./scatterfix-contest live \
  --work-dir temp/live --udp-output 127.0.0.1:1235 \
  --output-fps 1 --output-size 640x360
```

The same recovery path is available via `tsscatterfix`:

```sh
./tsscatterfix --mode contest --udp-input 1234 \
  --work-dir temp/live --udp-output 127.0.0.1:1235 --json-status
```

The last accepted frame is kept in memory and published continuously by the
standalone output thread, including while input stalls or after file EOF. Output
starts with a black frame until recovery succeeds. Ctrl-C stops it.
`--max-packets` bounds a capture and stops output at completion. `--output PATH`
writes a single recovered still as MPEG-TS to a file. This path sends recovered
images; normal-mode TS repair switches do not apply in contest mode.

`--work-dir`/`--output-dir` saves `best.png` and candidate elementary streams;
`--decode` additionally saves candidate PNGs. `--json-status-output PATH` writes
status JSON lines. No rolling TS file is needed for live analysis: the C engine
retains bounded PES/candidate buffers. Preserve input separately when collecting
reception evidence.

`--ml-model PATH` (`--model` alias) loads the existing `linear_regression_v1`
JSON format. Without a model, candidate ranking uses packet confidence, slice
bytes and continuity damage. `--vote` optionally tries weighted byte voting of
similarly sized keyframe slices. H.264 and H.265 are supported. General moving
video reconstruction is not implemented.

## Labelling and training in C

```sh
./scatterfix-model labels --features-csv /tmp/recovered/features.csv \
  --candidate-dir /tmp/recovered --output /tmp/recovered/labels.csv --idr-only
./scatterfix-model autolabel --labels-csv /tmp/recovered/labels.csv \
  --candidate-dir /tmp/recovered --output /tmp/recovered/auto-labels.csv
# Review/correct the labels; automatic labels are only a test-card heuristic.
./scatterfix-model train --features-csv /tmp/recovered/features.csv \
  --labels-csv /tmp/recovered/auto-labels.csv --output-model /tmp/recovered/model.json
```

Training requires at least three matching labelled rows. Repeat paired feature
and label arguments to combine datasets. Output parent directories must exist.

## Migration and limitations

The former `tools/contest_fragment_vote.py` and `tools/contest_live.py` paths are
replaced by `scatterfix-contest recover` and `scatterfix-contest live`.
Candidate filenames now use a sequence number and access-unit index. Python
analysis scheduling/rolling-file flags (`--interval`, `--startup-interval`,
`--analysis-packets`, `--append`, etc.) are replaced by incremental in-memory
analysis; they are not silently accepted. The old R-key reset remains in normal
TS mode; embedded image recovery has an explicit reset API. Stop/restart the
standalone contest tool to change station.

Local Python experiments present during migration were preserved under
`temp/legacy-python-20261029/` (ignored by Git), including the earlier offline
pixel-compositing prototype. They are archival references, not part of the
build or runtime. The C recovery path selects observed decodable frames and
optionally votes compressed fragments; it does not perform that older prototype's
per-pixel multi-frame compositing.

TSscatterfix is experimental. Keep original captures and verify results. It does
not provide FEC/demodulator recovery, arbitrary moving-video reconstruction or a
GUI toolkit. FFmpeg availability/licensing depends on the FFmpeg build used by
the embedding application. Historical contact notes remain in `QSO_LOG.md`;
connection-evidence procedure is in `CONNECTION_ANALYSIS_WORKFLOW.md`.
