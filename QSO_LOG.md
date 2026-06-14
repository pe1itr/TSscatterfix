# QSO Log

Operational notes for weak-signal DATV/TSscatterfix contacts. New connection
entries should follow `CONNECTION_ANALYSIS_WORKFLOW.md` so each QSO also
captures software optimization evidence.

## 2026-06-14 - DL5BCA / latest TS detection

- Counterstation: DL5BCA
- Logged: 2026-06-14 12:32 CEST
- Latest detection artifacts: `temp/live/`
- Capture analyzed: `temp/live/rolling.ts`
- Best recovered frame: `temp/live/best.png`
- Live analysis log: `temp/live/live.log`

### TS Detection

Analysis checked on 2026-06-14 12:35 CEST with:

```bash
./tsscatterfix --input temp/live/rolling.ts --dry-run --verbose --json-status --stats-interval 50 --mode contest
tools/contest_fragment_vote.py --input temp/live/rolling.ts --output-dir /tmp/tsscatterfix_latest_live_analysis --codec auto --decode --features-csv /tmp/tsscatterfix_latest_live_analysis/features.csv
```

Detected transport/service structure:

- TS packets read: 210
- PAT detected: yes
- PMT detected: yes, PMT PID `0x0102`
- SDT detected: yes
- SDT provider: `0303`
- SDT service name: `DL5BCA`
- PCR PID: `0x0100`
- Video PID: `0x0100`
- Audio PIDs: none detected
- Codec selected by fragment analysis: H.265/HEVC

Signal/packet quality evidence:

- Invalid TS headers: 3
- TEI packets: 2
- Continuity errors: 0
- Missing packets by continuity counter: 0
- Duplicates/out-of-order: 0/0
- Contest fragments seen/stored: 197
- Video fragments classified by contest mode: 158
- Repeated packet candidates: 50
- Repeated payload candidates: 63

Codec/keyframe evidence:

- Live analysis access units: 51
- Re-check access units: 64
- IDR/key units: 3
- VPS present: yes
- SPS present: yes
- PPS present: yes
- Candidate packet quality: 0 CC errors, 0 missing, 0 out-of-order, 0 bad PES
  starts for the selected keyframe candidates

Frame progression:

- AU 1: keyframe/IDR, score 6395, confidence 0.903, 29 packets, 4898 slice
  bytes, decode ok, full readable DL5BCA still frame.
- AU 2-25: non-key/inter-frame sequence with small slice payloads; not useful as
  standalone still-frame recovery evidence.
- AU 26: keyframe/IDR, score 6395, confidence 0.903, 29 packets, 4898 slice
  bytes, decode ok, full readable DL5BCA still frame. This was selected by the
  live analyzer as `best.png`.
- AU 27-50: repeated non-key/inter-frame sequence, matching the pattern after
  AU 1.
- AU 51: keyframe/IDR, live score 2398, confidence 0.909, 11 packets, 1822
  slice bytes, decode ok but visually partial: top line readable, lower image
  mostly blank.
- Interpreted progression: the stream produced two stable full still frames
  first, then a later degraded/partial keyframe. Holding AU 26 as best was the
  correct live behavior.

Frames available in `temp/live/analysis/`:

- `candidate_01_rolling_au_001.png`: decoded ok, full DL5BCA frame
- `candidate_02_rolling_au_026.png`: decoded ok, full DL5BCA frame; selected
  by the live analyzer
- `candidate_03_rolling_au_051.png`: decoded ok, partial/top-line DL5BCA frame
- `voted_01_bucket_26_rolling-au001_rolling-au026.png`: decoded ok, voted
  frame from AU 1 and AU 26

Selected live frame:

- `temp/live/best.png`
- Source selected in live log: `candidate_02_rolling_au_026.png`
- ML score in live log: 2.499
- Image quality in live log: 0.750
- Feature score for AU 26: 6395
- Packet confidence for AU 26: 0.903
- Packets for AU 26: 29
- Slice bytes for AU 26: 4898

Recovered image observation:

- The best and AU 26 frames visibly contain `0139 DL5BCA JO43FH 70cm`.
- The large received number is `0139`.
- The frame also shows `Sum: 13`.
- AU 51 still contains the top-line ID `0139 DL5BCA JO43FH`, but the lower
  image is mostly blank/partial.

Optimization notes:

- The latest-artifact search must prefer `temp/live/` over older files in
  `captures/`; otherwise a previous PA3AOD contact can be selected by mistake.
- Codec auto-detection selected H.265/HEVC correctly for this capture.
- AU 1 and AU 26 were both clean full frames with equal feature score; the live
  analyzer selected AU 26 based on the image-quality path.
- AU 51 decoded but was visually partial/mostly blank despite zero packet-level
  candidate errors; image-quality scoring should continue to penalize partial
  lower-frame content.
- Future optimization should track frame progression explicitly: once a full
  readable frame exists, later decoded frames should only replace it when the
  visual quality or information content improves.
- Voted AU 1/AU 26 decoded successfully; compare voted readability against the
  best direct decode instead of assuming the voted frame is automatically better.

Conclusion:

- This latest TS detection is the DL5BCA QSO evidence.
- The earlier PA3AOD capture in `captures/` was not the latest detection and
  should not be used for this DL5BCA entry.
