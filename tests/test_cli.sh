#!/bin/sh
set -eu
work=$(mktemp -d /tmp/scatterfix-cli.XXXXXX)
trap 'rm -rf "$work"' EXIT HUP INT TERM
./scatterfix-contest recover \
    --input captures/dl5bca_2026-06-14_123600_rolling.ts \
    --output-dir "$work/candidates" --decode --vote \
    --features-csv "$work/features.csv" \
    --json-status-output "$work/status.json" 2>"$work/recovery.log"
test -s "$work/candidates/best.png"
grep -q '"progress_percent":100' "$work/status.json"
grep -q '"service_name":"DL5BCA"' "$work/status.json"
grep -q '"provider_name":"0303"' "$work/status.json"
./scatterfix-model labels --features-csv "$work/features.csv" \
    --candidate-dir "$work/candidates" --output "$work/labels.csv" --idr-only
./scatterfix-model autolabel --labels-csv "$work/labels.csv" \
    --candidate-dir "$work/candidates" --output "$work/auto.csv"
./scatterfix-model train --features-csv "$work/features.csv" \
    --labels-csv "$work/auto.csv" --output-model "$work/model.json"
./scatterfix-contest recover --input captures/dl5bca_2026-06-14_123600_rolling.ts \
    --ml-model "$work/model.json" --output-dir "$work/trained" 2>"$work/trained.log"
test -s "$work/trained/best.png"
./tsscatterfix --mode contest \
    --input captures/on4vvv_2026-06-14_181433_70cm_rolling.ts \
    --output "$work/recovered.ts" --json-status-output "$work/live.json" 2>"$work/live.log"
test -s "$work/recovered.ts"
grep -q '"progress_percent":100' "$work/live.json"
./tsscatterfix --input captures/dl5bca_2026-06-14_123600_rolling.ts \
    --output "$work/pass.ts" --passthrough --stats-interval 0
cmp captures/dl5bca_2026-06-14_123600_rolling.ts "$work/pass.ts"
if ./tsscatterfix --input captures/dl5bca_2026-06-14_123600_rolling.ts \
    --output /dev/full --stats-interval 0 2>"$work/full.log"; then
    echo 'Expected output failure' >&2; exit 1
fi
if ./scatterfix-contest --max-mb -1 > /dev/null 2>&1; then
    echo 'Expected invalid configuration failure' >&2; exit 1
fi
if ./scatterfix-contest --ml-model "$work/features.csv" > /dev/null 2>&1; then
    echo 'Expected invalid model failure' >&2; exit 1
fi
printf '%s\n' 'All C CLI tests passed.'
