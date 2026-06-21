#!/usr/bin/env sh
set -eu

if [ "$(id -u)" -eq 0 ]; then
    echo "Do not run this verifier with sudo; it creates build outputs in the repo." >&2
    exit 1
fi

if ! command -v cmake >/dev/null 2>&1; then
    echo "cmake is required for verification." >&2
    exit 1
fi

if ! command -v g++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1; then
    echo "No C++ compiler found. Install g++ or clang++ with your system package manager." >&2
    exit 1
fi

if [ ! -f tester.md ]; then
    echo "tester.md is required for the Unicode verification benchmark." >&2
    exit 1
fi

cmake -S . -B build/linux-verify -DCMAKE_BUILD_TYPE=Release -DRAYOMD_USE_CURL=OFF
cmake --build build/linux-verify --config Release

BIN=build/linux-verify/rayomd

if command -v ldd >/dev/null 2>&1 && ldd "$BIN" 2>/dev/null | grep -q 'libcurl'; then
    echo "Default Linux verification build should not link libcurl; use -DRAYOMD_USE_CURL=ON only for URL image support." >&2
    exit 1
fi

mkdir -p benchmark-output/linux-verify

"$BIN" --export tester.md benchmark-output/linux-verify/defaults.pdf
test -s benchmark-output/linux-verify/defaults.pdf

if "$BIN" --export tester.md > benchmark-output/linux-verify/export-missing-args.txt 2>&1; then
    echo "Expected --export with a missing output path to fail." >&2
    exit 1
fi
grep -q -- "--export requires" benchmark-output/linux-verify/export-missing-args.txt
grep -q "Usage:" benchmark-output/linux-verify/export-missing-args.txt

if "$BIN" --export benchmark-output/linux-verify/missing.md benchmark-output/linux-verify/missing.pdf > benchmark-output/linux-verify/export-missing-input.txt 2>&1; then
    echo "Expected --export with a missing input file to fail." >&2
    exit 1
fi
grep -q "could not read input Markdown file" benchmark-output/linux-verify/export-missing-input.txt

"$BIN" --export tester.md benchmark-output/linux-verify/tester.pdf native elegant normal
"$BIN" --bench tester.md benchmark-output/linux-verify/bench-unicode 100 elegant normal

cat > benchmark-output/linux-verify/ascii.md <<'EOF'
# Linux Verify

This is an ASCII-only Markdown file for the portable standard-font path.

- alpha
- beta
- gamma

| Name | Value |
|---|---:|
| one | 1 |
EOF

"$BIN" --bench benchmark-output/linux-verify/ascii.md benchmark-output/linux-verify/bench-ascii 1000 elegant normal

echo
echo "Unicode benchmark:"
cat benchmark-output/linux-verify/bench-unicode/bench-results.txt
echo
echo "ASCII benchmark:"
cat benchmark-output/linux-verify/bench-ascii/bench-results.txt
echo
echo "Linux verification complete."
