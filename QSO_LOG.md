# QSO Log

Operational notes for weak-signal DATV/TSscatterfix contacts. New connection
entries should follow `CONNECTION_ANALYSIS_WORKFLOW.md` so each QSO also
captures software optimization evidence.

## 2026-06-14 - ON4VVV / latest TS detection

- Counterstation: ON4VVV
- Logged: 2026-06-14 17:38 CEST
- Latest detection artifacts: `temp/live/`
- Capture analyzed: `temp/live/rolling.ts`
- Preserved capture analyzed: `captures/on4vvv_2026-06-14_173335_rolling.ts`
- Best recovered frame: `captures/on4vvv_2026-06-14_173048_best.png`
- Selected clean keyframe candidate: `captures/on4vvv_2026-06-14_173335_candidate_au105.h264`
- Selected clean keyframe image: `captures/on4vvv_2026-06-14_173335_candidate_au105.png`
- Live analysis log: `temp/live/live.log`
- Operating note: contact reported on 51.7 MHz; recovered frame confirms
  `51.7` in the station card.

### TS Detection

Analysis checked on 2026-06-14 17:35-17:38 CEST with:

```bash
./tsscatterfix --input temp/live/rolling.ts --dry-run --verbose --json-status --stats-interval 50 --mode contest
tools/contest_fragment_vote.py --input temp/live/rolling.ts --output-dir /tmp/tsscatterfix_on4vvv_analysis --codec auto --decode --features-csv /tmp/tsscatterfix_on4vvv_analysis/features.csv --max-candidates 12 --vote
tools/contest_probe.py --input temp/live/rolling.ts
```

Detected transport/service structure:

- TS packets read: 6601
- PAT detected: yes
- PMT detected: yes, PMT PID `0x00ff`
- SDT detected: yes
- SDT provider: `Portsdown 4`
- SDT service name: `ON4VVV`
- PCR PID: `0x0100`
- Video PID: `0x0100`
- Audio PID: `0x0101`
- PMT stream types: H.264/AVC video `0x1b` on PID `0x0100`, AAC audio
  `0x0f` on PID `0x0101`
- Codec selected by fragment analysis: H.264/AVC

Signal/packet quality evidence:

- Invalid TS headers: 42
- TEI packets: 27 in the main parser; H.264 probe saw 0 TEI packets on the
  selected video PID
- Continuity errors: 194 overall
- Missing packets by continuity counter: 516 overall
- Duplicates/out-of-order: 7/42 overall
- Contest fragments seen/stored: 6525/4096
- Video fragments classified by contest mode: 5352
- PSI fragments classified by contest mode: 615
- Repeated packet candidates: 1422
- Repeated payload candidates: 1901
- H.264 probe saw 5383 video PID packets, 877 PES starts, 68 video continuity
  errors, 147 video missing-by-CC, 3 video duplicates, and 24 video
  out-of-order packets.

Codec/keyframe evidence:

- Access units from fragment analysis: 878
- IDR/key units: 88
- SPS present: yes
- PPS present: yes
- Decode trigger reached: yes, `SPS + PPS + IDR`
- NAL evidence from probe: 789 non-IDR slices, 88 IDR slices, 877 SEI units,
  93 SPS units, and 93 PPS units.
- A long repeated still-card pattern was present. The clean repeated frame
  group had 44 members with identical 8229-byte IDR payloads and zero candidate
  packet errors.

Frame progression:

- AU 2: IDR, score 3257, confidence 0.907, 15 packets, 2477 slice bytes,
  clean packet metrics; early partial keyframe.
- AU 70: IDR, score 9040, confidence 0.893, 40 packets, 7091 slice bytes,
  1 CC error; decodes to a mostly readable station card.
- AU 85: IDR, score 8948, confidence 0.903, 40 packets, 6888 slice bytes,
  clean packet metrics; readable but lower score than the later full stills.
- AU 105: IDR, score 10648, confidence 0.902, 47 packets, 8229 slice bytes,
  clean packet metrics; full readable ON4VVV frame and selected as the
  preserved direct candidate.
- AU 125/129: IDR, same score/confidence/packet metrics as AU 105; repeated
  full readable still frames.
- AU 141: IDR, score 12384, confidence 0.881, 58 packets, 10189 slice bytes,
  3 CC errors, 1 missing packet, and 2 out-of-order packets; high byte score
  but damaged and not better visually.
- AU 257-347: repeated clean full still frames matching AU 105.
- AU 407-497, 557-627, 642-652, 708-758, 776-786, 836-846, and AU 869:
  repeated clean full still frames matching AU 105.
- AU 826: IDR, score 13301, confidence 0.895, 59 packets, 10379 slice bytes,
  1 CC error; highest raw feature score but visually decoded as a gray/blank
  frame, so it must not replace the readable held frame.

Frames available:

- `temp/live/best.png`: live-selected readable frame.
- `captures/on4vvv_2026-06-14_173048_best.png`: preserved live-selected
  readable frame.
- `captures/on4vvv_2026-06-14_173335_candidate_au105.png`: preserved clean
  full readable direct candidate.
- `/tmp/tsscatterfix_on4vvv_analysis/voted_01_bucket_44_rolling-au105_rolling-au125_rolling-au129_rolling-au257_rolling-au267_rolling-au277_rolling-au287_rolling-au297_rolling-.png`:
  voted frame from the 44-member repeated clean still group; visually matches
  the direct readable frame.
- `/tmp/tsscatterfix_on4vvv_analysis/candidate_01_rolling_au_826.png`:
  highest raw score but visually blank/gray.

Recovered image observation:

- The recovered frame visibly contains `ON4VVV`.
- The large received number is `6045`.
- The locator line is `JO10WX`.
- The frequency text is `51.7`.
- The frame is full, clean, and suitable as QSO evidence.

Optimization notes:

- Codec auto-detection correctly selected H.264 from PMT stream type `0x1b`
  and H.264 NAL evidence.
- The ML/image-quality live analyzer selected AU 297 from the clean repeated
  still group; this was a better operational choice than the highest raw
  score candidate.
- Raw feature score alone is unsafe here: AU 826 scored highest because it had
  more slice bytes, but visual inspection shows a blank/gray decode. Candidate
  ranking should keep penalizing blank or low-detail decoded images even when
  packet and byte metrics look strong.
- Fragment voting did not need to repair the best frame; it confirmed the
  repeated 44-frame still pattern and matched the direct clean candidate.
- The stream contained many overall continuity gaps, invalid headers, and
  repeated payloads, but the useful still card repeated often enough that clean
  keyframes were recovered throughout the capture.

Conclusion:

- This latest TS detection is valid ON4VVV QSO evidence.
- Best readable content: `ON4VVV`, report/number `6045`, locator `JO10WX`,
  frequency `51.7`.
- The main recovery lesson is to prefer stable repeated clean still frames over
  later high-byte-score blank decodes.

## 2026-06-14 - ON1AFB / latest TS detection

- Counterstation: ON1AFB
- Logged: 2026-06-14 14:05 CEST
- Latest detection artifacts: `temp/live/`
- Capture analyzed: `temp/live/rolling.ts`
- Best recovered frame: `temp/live/best.png`
- Live analysis log: `temp/live/live.log`
- Path condition: very fast aircraft-scatter peaks; the QPSK demodulator had
  difficulty maintaining a stable transport stream during the short bursts.
  The signal was weakly present in the waterfall most of the time. Occasional
  aircraft-scatter peaks raised it, but moving notches across the passband
  caused fast flutter; some peaks produced only about 10 TS packets. Recovery
  depended on a favorable reflection that lasted long enough, or became strong
  enough, for the flutter to be dominated by signal strength.

### TS Detection

Analysis checked on 2026-06-14 14:01-14:05 CEST with:

```bash
./tsscatterfix --input temp/live/rolling.ts --dry-run --verbose --json-status --stats-interval 50 --mode contest
tools/contest_fragment_vote.py --input temp/live/rolling.ts --output-dir /tmp/tsscatterfix_connection_analysis --codec auto --decode --features-csv /tmp/tsscatterfix_connection_analysis/features.csv --max-candidates 8 --vote
tools/contest_probe.py --input temp/live/rolling.ts
```

Detected transport/service structure:

- TS packets read: 777
- PAT detected: yes
- PMT detected: yes, PMT PID `0x00ff`
- SDT detected: yes
- SDT provider: `Portsdown 4`
- SDT service name: `ON1AFB`
- PCR PID: `0x0100`
- Video PID: `0x0100`
- Audio PID: `0x0101`
- Codec selected by fragment analysis: H.264/AVC

Signal/packet quality evidence:

- Invalid TS headers: 40
- TEI packets: 25
- Continuity errors: 88
- Missing packets by continuity counter: 231
- Duplicates/out-of-order: 5/26
- Contest fragments seen/stored: 717/717
- Video fragments classified by contest mode: 531
- PSI fragments classified by contest mode: 74
- Repeated packet candidates: 149
- Repeated payload candidates: 192
- H.264 probe saw 534 video PID packets, 92 PES starts, 32 video continuity
  errors, 75 video missing-by-CC, 2 video duplicates, and 12 video out-of-order
  packets.

Codec/keyframe evidence:

- Access units from fragment analysis: 93
- IDR/key units: 6
- SPS present: yes
- PPS present: yes
- Decode trigger reached: yes, `SPS + PPS + IDR`
- The first available IDR decoded only to a uniform gray frame; later clean
  keyframes recovered the test card.

Frame progression:

- AU 24: IDR, score 4692, confidence 0.863, 28 packets, 4776 slice bytes,
  2 CC errors, 6 missing packets, 1 out-of-order packet, decode ok but visual
  state is blank/uniform gray.
- AU 39: IDR, score 9972, confidence 0.902, 44 packets, 7707 slice bytes,
  0 CC errors, 0 missing packets, 0 out-of-order packets, decode ok; full
  readable ON1AFB still frame and selected by the live analyzer.
- AU 49: IDR, score 7991, confidence 0.903, 35 packets, 6187 slice bytes,
  clean packet metrics, decode ok but visually partial/vertically smeared.
- AU 67: IDR, score 4687, confidence 0.854, 26 packets, 4334 slice bytes,
  3 CC errors, 2 missing packets, 2 out-of-order packets, decode ok but visual
  state is blank/uniform gray.
- AU 77: IDR, score 9497, confidence 0.902, 42 packets, 7334 slice bytes,
  clean packet metrics, decode ok; full readable frame matching AU 39.
- AU 92: IDR, score 1761, confidence 0.913, 8 packets, 1339 slice bytes,
  clean packet metrics but too little slice data; decode ok but blank/uniform
  gray.

Frames available:

- `temp/live/best.png`: live-selected readable frame from AU 39.
- `/tmp/tsscatterfix_connection_analysis/candidate_01_rolling_au_039.png`:
  readable full frame.
- `/tmp/tsscatterfix_connection_analysis/candidate_02_rolling_au_077.png`:
  readable full frame matching AU 39.
- `/tmp/tsscatterfix_connection_analysis/candidate_03_rolling_au_049.png`:
  partial/smeared frame.

Recovered image observation:

- The recovered frame visibly contains `ON1AFB`.
- The large received number is `4362`; the same number appears small at the top
  right.
- The locator line is `JO10QS`.
- The band/frequency text is `70_cm`.

Optimization notes:

- The early decode trigger behaved correctly: it tried as soon as
  `SPS + PPS + IDR` was present, but the quality gate rejected the first uniform
  gray frame.
- The connection demonstrates bursty aircraft-scatter behavior: useful keyframe
  evidence arrived in short peaks, while the QPSK demodulator produced many TS
  continuity gaps and invalid headers between peaks.
- Very short RF peaks may only contribute a handful of TS packets. Contest mode
  should keep collecting these bursts, but only accept a frame when a favorable
  reflection provides enough continuous keyframe bytes to overcome flutter.
- Do not lower the image-quality threshold for this case; it prevented a blank
  gray frame from becoming the held best frame.
- Candidate ranking worked well once clean keyframes arrived: AU 39 and AU 77
  had zero candidate packet errors and full readable images, while damaged or
  too-short IDRs decoded to blank/partial frames.
- For this type of fast aircraft-scatter path, continue aggressive startup
  analysis but hold the best readable frame through later short or blank
  keyframes.

Conclusion:

- This latest TS detection is valid ON1AFB QSO evidence.
- Best readable content: `ON1AFB`, report/number `4362`, locator `JO10QS`,
  band `70_cm`.
- The main limiting factor was not lack of codec anchors but burst instability
  in the demodulated QPSK stream during fast aircraft-scatter peaks.

## 2026-06-14 - F5RZC / latest TS detection

- Counterstation: F5RZC
- Logged: 2026-06-14 12:57 CEST
- Latest detection artifacts: `temp/live/`
- Preserved capture analyzed: `captures/f5rzc_2026-06-14_125753_rolling.ts`
- Best recovered frame: `captures/f5rzc_2026-06-14_125617_best.png`
- Selected clean keyframe candidate: `captures/f5rzc_2026-06-14_125753_candidate_au206.h264`
- Selected clean keyframe image: `captures/f5rzc_2026-06-14_125753_candidate_au206.png`
- Voted readable frame: `captures/f5rzc_2026-06-14_125753_voted_bucket21.png`
- Live analysis log: `temp/live/live.log`

### TS Detection

Analysis checked on 2026-06-14 13:00 CEST with:

```bash
./tsscatterfix --input temp/live/rolling.ts --dry-run --verbose --json-status --stats-interval 50 --mode contest
tools/contest_fragment_vote.py --input temp/live/rolling.ts --output-dir /tmp/tsscatterfix_f5rzc_analysis --codec auto --decode --features-csv /tmp/tsscatterfix_f5rzc_analysis/features.csv
```

Detected transport/service structure:

- TS packets read: 1498
- PAT detected: yes
- PMT detected: yes, PMT PID `0x1000`
- SDT detected: yes
- SDT provider: `FIRM2101RC`
- SDT service name: `F5RZC`
- PCR PID: `0x0100`
- Video PID: `0x0100`
- Audio PIDs: none detected
- Codec selected by fragment analysis: H.264/AVC

Signal/packet quality evidence:

- Invalid TS headers: 25
- TEI packets: 15
- Continuity errors: 142
- Missing packets by continuity counter: 438
- Duplicates/out-of-order: 3/42
- Contest fragments seen/stored: 1463/1463
- Video fragments classified by contest mode: 1014
- PSI fragments classified by contest mode: 414
- Repeated packet candidates: 449
- Repeated payload candidates: 689

Codec/keyframe evidence:

- Access units from re-analysis: 326
- IDR/key units: 28
- SPS present: yes
- PPS present: yes
- Candidate packet quality varies strongly: the best byte-score candidates
  AU 316 and AU 279 had continuity/missing/out-of-order damage, while the
  clean repeated still frames at AU 135, AU 206, and AU 295 had zero candidate
  packet errors.

Frame progression:

- AU 3: IDR, score 4687, confidence 0.883, 22 packets, 3800 slice bytes,
  1 CC error and 1 out-of-order packet; decodes partially.
- AU 18: IDR, score 5166, confidence 0.904, 23 packets, 3976 slice bytes,
  clean candidate; full readable F5RZC still frame.
- AU 87/123/153/177/234: repeated clean full still frames matching AU 18.
- AU 135: IDR, score 5608, confidence 0.904, 25 packets, 4316 slice bytes,
  clean candidate; full readable still frame and selected by the live analyzer.
- AU 206: IDR, score 5609, confidence 0.904, 25 packets, 4317 slice bytes,
  clean candidate; full readable still frame and preserved as the selected
  clean keyframe artifact.
- AU 218: IDR, score 5223, confidence 0.904, 23 packets, 4033 slice bytes,
  clean packet metrics but visible vertical smearing in the lower text region.
- AU 279: IDR, score 7617, confidence 0.849, 45 packets, 7873 slice bytes,
  damaged candidate with 5 CC errors, 9 missing packets, and 2 out-of-order
  packets; decodes worse than the clean repeated stills.
- AU 295: IDR, score 5609, confidence 0.904, 25 packets, 4317 slice bytes,
  clean candidate; full readable still frame.
- AU 316: IDR, score 7790, confidence 0.875, 42 packets, 7345 slice bytes,
  highest feature score but damaged candidate with 2 CC errors, 7 missing
  packets, and 1 out-of-order packet; top text is readable but the lower line
  is smeared/partially lost.

Frames available:

- `temp/live/best.png`: live-selected full readable frame.
- `captures/f5rzc_2026-06-14_125753_candidate_au206.png`: clean
  full readable frame.
- `/tmp/tsscatterfix_f5rzc_analysis/candidate_04_rolling_au_295.png`: clean
  full readable frame.
- `/tmp/tsscatterfix_f5rzc_analysis/candidate_07_rolling_au_018.png`: clean
  full readable frame.
- `captures/f5rzc_2026-06-14_125753_voted_bucket21.png`:
  voted frame, full readable and visually good.

Recovered image observation:

- The recovered frame visibly contains `F5RZC`.
- The large received number is `1964`; the same number also appears at the top
  right.
- The locator line is `JO10AR`.
- The band/frequency text is `70_cm`.
- The full still frames are clean enough for QSO evidence; the late highest
  score candidates should not replace them.

Optimization notes:

- Codec auto-detection correctly selected H.264/AVC for this QSO, unlike the
  earlier H.265 DL5BCA case.
- Packet/byte feature score over-ranked AU 316 and AU 279 even though visual
  readability was worse. Penalize candidates with nonzero missing/CC/out-of-order
  counts before choosing a later high-byte IDR.
- The clean still appears repeatedly across the QSO, so repeated clean frames
  should be preferred over damaged late frames with larger slice byte counts.
- The voted AU 18/87/123/153/177/218/234 frame was visually as good as the best
  direct full still, while voted bucket 23 from AU 135/206/256/295 produced a
  tiny/failed-looking PNG in the live artifacts. Voting quality needs visual or
  structural validation per bucket.

Conclusion:

- This latest TS detection is the F5RZC QSO evidence.
- Best readable content: `F5RZC`, report/number `1964`, locator `JO10AR`,
  band `70_cm`.
- For software tuning, this is a useful negative example for pure feature-score
  ranking: the clean repeated middle AUs beat the damaged late high-score AUs.

## 2026-06-13 - PE1ORG / H.265 discovery context

- Counterstation: PE1ORG
- Logged: 2026-06-13, exact time not preserved
- Capture artifacts: not preserved; no PE1ORG files remain in `temp/live/` or
  `captures/`
- Historical importance: this QSO triggered the investigation that revealed the
  incoming DATV stream could be H.265/HEVC rather than H.264.

Notes:

- Keep this entry as context for the codec auto-detection work, even though the
  original capture is gone.
- Later DL5BCA evidence on 2026-06-14 confirmed the H.265/HEVC handling path
  with preserved artifacts.

## 2026-06-14 - DL5BCA / latest TS detection

- Counterstation: DL5BCA
- Logged: 2026-06-14 12:32 CEST
- Latest detection artifacts: `captures/`
- Capture analyzed: `captures/dl5bca_2026-06-14_123600_rolling.ts`
- Best recovered frame: `captures/dl5bca_2026-06-14_122828_best.png`
- Selected keyframe candidate: `captures/dl5bca_2026-06-14_122828_candidate_au026.h265`
- Live analysis log: `temp/live/live.log`

### TS Detection

Analysis checked on 2026-06-14 12:35 CEST with:

```bash
./tsscatterfix --input captures/dl5bca_2026-06-14_123600_rolling.ts --dry-run --verbose --json-status --stats-interval 50 --mode contest
tools/contest_fragment_vote.py --input captures/dl5bca_2026-06-14_123600_rolling.ts --output-dir /tmp/tsscatterfix_latest_live_analysis --codec auto --decode --features-csv /tmp/tsscatterfix_latest_live_analysis/features.csv
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

- `captures/dl5bca_2026-06-14_122828_best.png`
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
