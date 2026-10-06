#!/usr/bin/env bash
# Decode with HM; discard reconstructed pixels and keep diagnostics by input.
set -euo pipefail

if [[ $# -ne 1 ]]; then
    printf 'Usage: %s input.h265\n' "$0" >&2
    exit 1
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
decoder="$script_dir/h265nal/HM/bin/TAppDecoderStaticd"
input="$1"
log="${input%.h265}_hm_decode.log"

if [[ ! -f "$input" || ! -r "$input" || ! -s "$input" ]]; then
    printf 'Input is missing, unreadable or empty: %s\n' "$input" >&2
    exit 1
fi
if [[ ! -x "$decoder" ]]; then
    printf 'HM decoder not found or not executable: %s\n' "$decoder" >&2
    exit 1
fi

# Avoid leaving core dumps if HM asserts on a malformed bitstream.
ulimit -c 0
printf 'Decoding with HM: %s\n' "$input"
status=0
"$decoder" -b "$input" -o /dev/null > "$log" 2>&1 || status=$?

frames=$(awk '/^[[:space:]]*POC[[:space:]]+-?[0-9]+/ { count++ }
    END { print count+0 }' "$log")
errors=$(awk '/(^|[^[:alnum:]_])(ERROR|Error|error)([^[:alnum:]_]|$)|Assertion.*failed/ {
    count++
} END { print count+0 }' "$log")

if [[ "$status" -ne 0 || "$frames" -eq 0 || "$errors" -ne 0 ]]; then
    printf 'FAIL: HM exit=%s, decoded pictures=%s, error lines=%s\nLog: %s\n' \
        "$status" "$frames" "$errors" "$log" >&2
    if [[ "$status" -ne 0 ]]; then
        exit "$status"
    fi
    exit 1
fi

printf 'PASS: HM decoded %s pictures.\nLog: %s\n' "$frames" "$log"
