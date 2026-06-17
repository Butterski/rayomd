# Contributing

RayoMD is meant to stay small, fast, and easy to ship. Contributions are
welcome when they preserve that shape.

## Project Priorities

- Keep the native path dependency-light.
- Keep startup, conversion time, and release size visible when changing hot code.
- Treat native rendering as a fast Markdown subset, not a full Pandoc clone.
- Make missing images, unsupported inputs, and unavailable URLs degrade cleanly.
- Keep generated PDFs, benchmark corpora, build trees, and local binaries out of
  source.

## Build

Windows:

```sh
cmake -S . -B build/windows -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows --config Release
```

Linux / WSL:

```sh
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux --config Release
```

Linux dependencies are listed in `README.md`.

## Verify

Run the lightweight Linux verifier when possible:

```sh
sh scripts/verify-linux.sh
```

For Windows changes, build the release target and run a benchmark smoke:

```sh
build/windows/rayomd.exe --bench tester.md benchmark-output/manual 100 modern normal
```

For performance-sensitive work, run `scripts/perf_watch.py` before and after the
change and report the platform, storage location, suite, and headline deltas.

## Versioning

The project version lives in `VERSION` and is compiled into both binaries.
Update it when preparing a tagged release, and verify:

```sh
build/linux/rayomd --version
build/windows/rayomd.exe --version
```

Use semantic versioning where practical:

- Patch: bug fixes, docs, packaging, verification changes.
- Minor: new supported Markdown features, CLI modes, or API additions.
- Major: breaking CLI/API behavior or package contract changes.

## Pull Requests

- Keep PRs focused.
- Explain user-visible behavior changes.
- Include verification commands and benchmark numbers when relevant.
- Update `README.md`, `AGENTS.md`, and `VERSION` only when the change affects
  users, maintainers, or releases.
- Do not edit vendored code in `third_party/` unless the task is explicitly a
  dependency update.
