#!/bin/sh
set -eu

program=${1:-./data_upload_scan}
test_root=$(mktemp -d)
trap 'rm -rf "$test_root"' EXIT INT TERM

mkdir -p "$test_root/a/b" "$test_root/System Volume Information"
: > "$test_root/one.mcap"
: > "$test_root/a/TWO.MCAP"
: > "$test_root/a/b/broken.mcap.incomplete"
: > "$test_root/a/b/not_mcap.txt"
: > "$test_root/System Volume Information/ignored.mcap"
ln -s "$test_root/a" "$test_root/link_to_a"

output=$($program --root "$test_root" --limit 20)
printf '%s\n' "$output"
printf '%s\n' "$output" | grep -q 'complete=2 incomplete=1 total=3'

complete_output=$($program --root "$test_root" --complete-only --count-only)
printf '%s\n' "$complete_output" | grep -q 'complete=2 incomplete=0 total=2'

echo '[TEST] PASS'
