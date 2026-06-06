#!/usr/bin/env bash
set -euo pipefail

source_dir="${1:-/mnt/e/projekty/fast-markdown/imgui-version}"
label="${2:-$(date +%Y%m%d_%H%M%S)}"
work_dir="$(mktemp -d "${HOME}/fast-md-commercial.XXXXXX")"
dest_dir="${source_dir}/benchmark-output/commercial/linux_ext4_${label}"

printf 'EXT4_WORKDIR=%s\n' "$work_dir"

rsync -a --no-owner --no-group \
  --exclude=.git \
  --exclude=build \
  --exclude=benchmark-output \
  "$source_dir/" "$work_dir/"

cd "$work_dir"
cmake -S . -B build/linux-verify -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/linux-verify -j 4 >/dev/null

python3 scripts/commercial_benchmark.py \
  --root benchmark-output/commercial \
  --platform linux \
  --bin build/linux-verify/fast-markdown \
  --label "$label" \
  --generate \
  --run

mkdir -p "$dest_dir"
mkdir -p "$dest_dir/reports"
cp -a benchmark-output/commercial/reports/. "$dest_dir/reports/"
if [[ -d benchmark-output/commercial/results ]]; then
  rsync -a \
    --include='*/' \
    --include='*.json' \
    --include='*.txt' \
    --exclude='*' \
    benchmark-output/commercial/results "$dest_dir/"
fi
printf 'COPIED_RESULTS=%s\n' "$dest_dir"
