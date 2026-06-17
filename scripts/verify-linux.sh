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

cmake -S . -B build/linux-verify -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux-verify --config Release

BIN=build/linux-verify/rayomd

mkdir -p benchmark-output/linux-verify

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
