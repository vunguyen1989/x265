#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    printf 'Usage: %s input.h265\n' "$0" >&2
    exit 1
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
parser="$script_dir/h265nal/build/tools/h265nal"
input="$1"
log="${input%.h265}_h265nal.log"

if [[ ! -f "$input" || ! -r "$input" ]]; then
    printf 'Cannot read input: %s\n' "$input" >&2
    exit 1
fi
if [[ ! -x "$parser" ]]; then
    printf 'Parser binary not found or not executable: %s\n' "$parser" >&2
    exit 1
fi

"$parser" --dump-all --no-as-one-line -i "$input" > "$log" 2>&1
printf 'Log: %s\n' "$log"
