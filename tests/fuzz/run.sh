#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Runs every fuzz target of a RAYOMD_FUZZ=ON build for SECONDS each, one after the other, on the
# seed corpus (tests/fuzz/seeds.py collect), and fails when any of them finds an input that
# crashes, trips a sanitizer, runs past 10 seconds or past 2 GiB.
#
#   tests/fuzz/run.sh BUILD_DIR SECONDS [TARGET...] [-- LIBFUZZER_FLAGS...]
#
# The corpus grows in BUILD_DIR/fuzz-corpus/<target>; a failing input lands in
# BUILD_DIR/fuzz-artifacts/<target>-crash-* (or -timeout-, -oom-, -leak-).
set -eu

if [ $# -lt 2 ]; then
    echo "usage: $0 BUILD_DIR SECONDS [TARGET...] [-- LIBFUZZER_FLAGS...]" >&2
    exit 2
fi
build=$1
seconds=$2
shift 2
here=$(cd "$(dirname "$0")" && pwd)
targets=""
while [ $# -gt 0 ] && [ "$1" != "--" ]; do
    targets="$targets $1"
    shift
done
[ $# -gt 0 ] && shift
[ -n "$targets" ] || targets="buildpdf pdfsource theme font cff image svg mermaid"

corpus=$build/fuzz-corpus
artifacts=$build/fuzz-artifacts
mkdir -p "$artifacts"
if [ ! -d "$corpus" ]; then
    python3 "$here/seeds.py" collect "$corpus"
fi
# Stacks with file and line: llvm-symbolizer, else binutils' addr2line.
if ! command -v llvm-symbolizer >/dev/null 2>&1 && [ -z "${ASAN_SYMBOLIZER_PATH:-}" ] && command -v addr2line >/dev/null 2>&1; then
    ASAN_SYMBOLIZER_PATH=$(command -v addr2line)
    export ASAN_SYMBOLIZER_PATH
fi

failed=""
for target in $targets; do
    case $target in
        buildpdf) max_len=131072 dict=markdown.dict ;;
        pdfsource) max_len=262144 dict="" ;;
        theme) max_len=8192 dict="" ;;
        font) max_len=65536 dict="" ;;
        cff) max_len=16384 dict="" ;;
        image) max_len=65536 dict="" ;;
        svg) max_len=65536 dict=svg.dict ;;
        mermaid) max_len=16384 dict=mermaid.dict ;;
        *) echo "unknown target $target" >&2; exit 2 ;;
    esac
    mkdir -p "$corpus/$target"
    echo "== $target: $seconds s"
    if ! "$build/tests/fuzz/rayomd-fuzz-$target" "$corpus/$target" \
        -max_total_time="$seconds" -timeout=10 -rss_limit_mb=2048 -max_len="$max_len" \
        -print_final_stats=1 -artifact_prefix="$artifacts/$target-" \
        ${dict:+-dict="$here/$dict"} "$@"; then
        failed="$failed $target"
    fi
done
if [ -n "$failed" ]; then
    echo "fuzz targets that failed:$failed" >&2
    ls -l "$artifacts" >&2
    exit 1
fi
