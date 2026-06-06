#!/usr/bin/env bash
set -euo pipefail

source_dir="${1:-/mnt/e/projekty/fast-markdown/imgui-version}"
label="${2:-ext4_baseline}"
work_dir="$(mktemp -d "${HOME}/fast-md-bench.XXXXXX")"

printf 'EXT4_WORKDIR=%s\n' "$work_dir"

rsync -a --no-owner --no-group \
  --exclude=.git \
  --exclude=build \
  --exclude=benchmark-output \
  "$source_dir/" "$work_dir/"

cd "$work_dir"
cmake_args=()
if [[ -n "${FAST_MARKDOWN_CMAKE_ARGS:-}" ]]; then
  # Space-separated flags are enough for this benchmark helper.
  read -r -a cmake_args <<< "$FAST_MARKDOWN_CMAKE_ARGS"
fi

cmake -S . -B build/linux-verify -DCMAKE_BUILD_TYPE=Release "${cmake_args[@]}" >/dev/null
cmake --build build/linux-verify -j 4 >/dev/null
python3 scripts/stress_benchmark.py \
  --generate \
  --run \
  --label "$label" \
  --bin build/linux-verify/fast-markdown
