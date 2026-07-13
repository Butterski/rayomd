# Performance development

Correctness and performance are intentionally separate. Build RayoMD first,
run `tests/verify_cli.py` for behavior, and use `tools/benchmark.py` only when
timing or release evidence is needed. Raw reports belong under ignored
`benchmark-output/`.

## Maintained workflows

```sh
python tools/benchmark.py run -- --binary build/windows/rayomd.exe --platform windows --suite watch --label local
python3 tools/benchmark.py run -- --binary build/linux/rayomd --platform linux-native-ext4 --suite watch --label local
python tools/benchmark.py reversible -- --binary build/windows/rayomd.exe --platform windows --suite full --samples 5
python tools/benchmark.py compare -- --rayomd build/windows/rayomd.exe --root benchmark-output/pandoc --runs 5
python tools/benchmark.py competitors -- --rayomd build/windows/rayomd.exe --root benchmark-output/competitors
python3 tools/benchmark.py release -- --from-version 1.1.0 --suite quick
```

The standard `run` workflow covers cold export, warm `--bench`, folder batch,
stdin batch, warm serve, ASCII, Unicode, tables, explicit rules/page breaks,
comments, and optional local images. Use it to protect the default fast path.

The `reversible` workflow covers all source-recovery work in one reproducible
run:

- ordinary versus embedded output at 0 B, 10 KiB, 1 MiB, and the 10 MiB cap;
- ASCII, Unicode, links, local image, failed/remote fallback, table/list,
  multipage, and source-only/private-content documents;
- folder batch, stdin batch, and warm serve, embedding both off and on;
- inspect and byte-exact recovery p50/p95;
- not-reversible, unsupported-profile, integrity, truncation, and source-limit
  rejection latency;
- output size, optional psutil peak RSS, raw-versus-Flate fixtures, qpdf and
  Poppler availability, and optional allocation deltas from a profiling build.

`quick` omits the 10 MiB scale case. `full` includes it. Both retain feature,
workflow, rejection, and storage coverage. Use at least five samples for a
publishable p95; the harness caps the maximum-size case at three runs.
Each child command is terminated after 120 seconds by default so a broken CLI
route cannot leave the benchmark apparently idle for an hour. Override this
only for deliberately slow environments with `--timeout-seconds N`.

To collect allocation deltas without changing the release binary:

```sh
cmake -S . -B build/profile -DCMAKE_BUILD_TYPE=Release -DRAYOMD_ENABLE_PROFILING=ON
cmake --build build/profile --config Release
python tools/benchmark.py reversible -- \
  --binary build/windows/rayomd.exe \
  --profile-binary build/profile/rayomd.exe \
  --platform windows --suite full --samples 5
```

## Regression and size gate

Record the baseline before changing performance-sensitive code. Compare an
identically configured candidate with the explicit record, not a local-history
guess:

```sh
python tools/benchmark.py run -- \
  --binary build/before/rayomd.exe --platform windows --suite watch --label before

python tools/benchmark.py run -- \
  --binary build/after/rayomd.exe --platform windows --suite watch --label after \
  --baseline-record benchmark-output/perf-watch/windows/<before-run>/record.json \
  --fail-on-slower-pct 5
```

Also compare executable/package byte size. A change that improves a tiny case
but regresses larger documents, batch/serve throughput, memory, or package size
must be fixed, isolated behind an opt-in/separate package, or documented as a
deliberate tradeoff.

Keep the candidate comparable to its baseline: same suite, seed, style, margin,
image mode, worker count, compiler, build type, and storage. Linux release
claims should use native/ext4 storage. Label WSL `/mnt/*` results explicitly.

## Keeping the release light

- Build `Release`; never benchmark Debug binaries.
- Keep curl, simdutf, profiling, TSAN, LTO, and PGO off in the default package.
- On Linux, disable zlib discovery only when the smallest portable artifact is
  more important than PNG alpha decoding.
- Keep embedding opt-in. It has no dependency cost, but increases every
  reversible PDF by the exact source plus profile overhead.
- Keep remote image fetching opt-in and exclude network latency from stable
  performance claims.
- Keep generated corpora, PDFs, reports, profiling binaries, and build trees out
  of source control.
- Do not raise the 10 MiB reversible limit without maximum-case output/RSS
  measurements on both platforms.

The router retains focused implementations under `scripts/`. Curated, dated
release records live under `docs/benchmarks/`; raw JSON and artifacts do not.
