# Performance development

Correctness and performance are intentionally separate. Build RayoMD first,
run tests/verify_cli.py for behavior, and use tools/benchmark.py only when
timing or release evidence is needed. Raw reports belong under ignored
benchmark-output/.

Commands:

    python tools/benchmark.py run -- --binary build/windows/rayomd.exe --platform windows --suite watch --label local
    python3 tools/benchmark.py run -- --binary build/linux/rayomd --platform linux-wsl --suite watch --label local
    python tools/benchmark.py compare -- --rayomd build/windows/rayomd.exe --root benchmark-output/pandoc --runs 5
    python tools/benchmark.py competitors -- --rayomd build/windows/rayomd.exe --root benchmark-output/competitors
    python3 tools/benchmark.py release -- --from-version 1.1.0 --suite quick

The router retains focused implementation helpers under scripts/. Curated,
dated release records live under docs/benchmarks/releases/. Synthetic corpora,
PDFs, and raw timing reports are never tracked.