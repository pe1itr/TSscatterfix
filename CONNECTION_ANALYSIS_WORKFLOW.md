# Connection Analysis Workflow

Use this workflow whenever a connection or QSO must be analyzed. The goal is not
only to document the contact, but to collect repeatable evidence for future
TSscatterfix optimization.

## Objective

Every analyzed connection should answer four questions:

- What did the transport stream identify? Service name, provider, PIDs, codec,
  and usable metadata.
- How did the received frames evolve over time? Access-unit sequence, repeated
  GOP pattern, first usable frame, best frame, regressions, and partial frames.
- Which frames were actually recoverable? Best frame, candidates, voted frames,
  partial frames, and visible text.
- Where did recovery succeed or fail? Continuity, TEI, invalid TS headers, codec
  anchors, decode failures, and candidate ranking.
- What should be improved in TSscatterfix? Concrete observations that can become
  code, model, or workflow changes.

## Source Selection

Always start from the most recent live detection unless the user explicitly names
a capture.

Preferred search order:

1. `temp/live/`
2. Other recently modified `temp/live*` or `/tmp/tsscatterfix*` analysis
   directories
3. `captures/`
4. User-specified files

Before analyzing, list candidate artifacts by modification time and state which
one is selected. Do not reuse an older capture just because it is in `captures/`.

Important files to look for:

- `rolling.ts`
- `best.png`
- `live.log`
- `analysis/features.csv`
- `analysis/candidate_*.png`
- `analysis/candidate_*.h264` or `analysis/candidate_*.h265`
- `analysis/voted_*.png`
- `analysis/voted_*.h264` or `analysis/voted_*.h265`

## Required Commands

Run the TS parser in contest mode:

```bash
./tsscatterfix --input PATH/rolling.ts --dry-run --verbose --json-status --stats-interval 50 --mode contest
```

Run fragment/keyframe analysis:

```bash
tools/contest_fragment_vote.py \
  --input PATH/rolling.ts \
  --output-dir /tmp/tsscatterfix_connection_analysis \
  --codec auto \
  --decode \
  --features-csv /tmp/tsscatterfix_connection_analysis/features.csv
```

If the codec or PID is ambiguous, rerun with explicit `--video-pid` or
`--codec h264|h265` and document why.

For H.264 captures, optionally run:

```bash
tools/contest_probe.py --input PATH/rolling.ts
```

## Required Evidence

Document these TS fields:

- capture path and artifact directory
- analysis time
- TS packets read
- PAT/PMT/SDT presence
- service name and provider
- PMT PID, PCR PID, video PID, audio PIDs
- codec selected or inferred
- stream type when available

Document these signal-quality fields:

- invalid TS headers
- TEI packet count
- continuity errors
- missing-by-CC count
- duplicate and out-of-order counts
- repeated packet and payload candidates
- contest fragments seen/stored
- video/audio/PSI fragment counts

Document these codec/frame fields:

- access units
- key/IDR units
- VPS/SPS/PPS presence as applicable
- frame/access-unit progression from first received AU to last analyzed AU
- which AUs are keyframes, non-keyframes, full decodes, partial decodes, blanks,
  or failed decodes
- score/confidence trend over the access-unit sequence
- whether repeated GOPs or repeated still-card patterns are visible
- selected candidate access unit
- candidate score and confidence
- packet count and slice bytes
- candidate CC errors, missing, out-of-order, and bad PES starts
- decode status for each usable frame
- whether voted frames improved, matched, or worsened the result

Document visible frame content:

- callsign
- locator
- band/frequency text
- report or contest number
- checksum/sum text
- whether the frame is full, partial, blank, corrupted, or ambiguous

## Frame Progression

The frame progression is the main learning artifact. Do not only report the best
frame. Describe what arrived before and after it, because TSscatterfix
optimization depends on understanding when evidence appears, repeats, improves,
or degrades.

For each connection, include a compact timeline table with:

- access unit
- keyframe/IDR flag
- score and packet confidence
- packet count and slice bytes
- packet errors for that candidate
- decode result
- visual state
- operational meaning

Example:

```text
AU 001 key score=6395 conf=0.903 packets=29 slice=4898 decode=ok visual=full
AU 002-025 non-key low-byte delta frames; no standalone still recovery expected
AU 026 key score=6395 conf=0.903 packets=29 slice=4898 decode=ok visual=full
AU 027-050 non-key repeat of previous inter-frame pattern
AU 051 key score=2398 conf=0.909 packets=11 slice=1822 decode=ok visual=partial
```

When possible, summarize the pattern:

- first usable full frame
- best live-selected frame
- last usable frame
- whether later frames got better, stayed stable, or degraded
- whether voting combined equivalent frames or repaired a damaged frame
- whether image-quality scoring agreed with visual inspection

## Software Learning Notes

Every connection entry must include a short `Optimization notes` section. Use
specific observations rather than general impressions.

Good examples:

- Codec auto-detection switched correctly from PMT stream type to H.265.
- Live analyzer selected AU 26 over AU 1 despite equal feature score; compare
  image-quality ranking inputs.
- AU 51 decoded but only contained the top line; add a penalty for mostly blank
  lower image regions.
- Frame progression shows stable keyframes at AU 1 and AU 26, then degradation
  at AU 51; use the progression to avoid replacing a good held frame with a
  later partial decode.
- PSI lock reached confidence 1.000 after 200 packets; earlier frame selection
  should not depend on full PSI confidence.
- Voted frame matched AU 1/AU 26 and did not improve readability; avoid treating
  voting as automatically better than the best direct decode.

Bad examples:

- Worked well.
- Need better recovery.
- Signal was weak.

## QSO Log Template

Use this structure in `QSO_LOG.md`:

````markdown
## YYYY-MM-DD - CALLSIGN / latest TS detection

- Counterstation: CALLSIGN
- Logged: YYYY-MM-DD HH:MM TZ
- Latest detection artifacts: `PATH/`
- Capture analyzed: `PATH/rolling.ts`
- Best recovered frame: `PATH/best.png`
- Live analysis log: `PATH/live.log`

### TS Detection

Commands run:

```bash
...
```

Detected transport/service structure:

- ...

Signal/packet quality evidence:

- ...

Codec/keyframe evidence:

- ...

Frame progression:

- ...

Frames available:

- ...

Recovered image observation:

- ...

Optimization notes:

- ...

Conclusion:

- ...
````

## Handling Mismatches

If the user names a callsign but the latest TS evidence shows another station:

- do not force the log to match the user-supplied callsign;
- document the mismatch explicitly;
- continue searching newer artifacts before concluding;
- only use older captures when the latest artifacts are absent or the user asks
  for that specific capture.

If a frame is visually readable but TS metadata disagrees with it, document both
and mark the conflict as an optimization/debugging item.
