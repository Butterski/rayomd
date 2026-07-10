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

if [ ! -f tester.md ] || [ ! -f docs/benchmark_smoke.md ]; then
    echo "tester.md and docs/benchmark_smoke.md are required for verification." >&2
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

printf '# Stdin Verify

Hello **stdin**.
' | "$BIN" --stdin benchmark-output/linux-verify/stdin.pdf native modern normal
test -s benchmark-output/linux-verify/stdin.pdf

printf '# Stdin Image Fallback

![stdin-local](relative.png)
' | "$BIN" --stdin benchmark-output/linux-verify/stdin-no-extension native elegant normal
test -s benchmark-output/linux-verify/stdin-no-extension.pdf
grep -a -q "stdin-local" benchmark-output/linux-verify/stdin-no-extension.pdf

if printf '# Missing Output
' | "$BIN" --stdin > benchmark-output/linux-verify/stdin-missing-args.txt 2>&1; then
    echo "Expected --stdin with a missing output path to fail." >&2
    exit 1
fi
grep -q -- "--stdin requires" benchmark-output/linux-verify/stdin-missing-args.txt
grep -q "Usage:" benchmark-output/linux-verify/stdin-missing-args.txt

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

SECURITY_DIR=benchmark-output/linux-verify/security
mkdir -p "$SECURITY_DIR/doc" "$SECURITY_DIR/out"
cp docs/assets/rayomd.png "$SECURITY_DIR/doc/allowed.png"
cp docs/assets/rayomd.png "$SECURITY_DIR/outside.png"

cat > "$SECURITY_DIR/doc/allowed.md" <<'EOF'
# Allowed Local Image

![allowed-local](allowed.png)
EOF
"$BIN" --export "$SECURITY_DIR/doc/allowed.md" "$SECURITY_DIR/out/allowed.pdf"
test -s "$SECURITY_DIR/out/allowed.pdf"
if grep -a -q "allowed-local" "$SECURITY_DIR/out/allowed.pdf"; then
    echo "Expected in-directory local image to render instead of fallback text." >&2
    exit 1
fi

cat > "$SECURITY_DIR/doc/escape.md" <<'EOF'
# Escaping Local Image

![blocked-local](../outside.png)
EOF
"$BIN" --export "$SECURITY_DIR/doc/escape.md" "$SECURITY_DIR/out/escape.pdf"
grep -a -q "blocked-local" "$SECURITY_DIR/out/escape.pdf"

cat > "$SECURITY_DIR/doc/url.md" <<'EOF'
# URL Image

![blocked-url](http://127.0.0.1:9/image.png)
EOF
"$BIN" --export "$SECURITY_DIR/doc/url.md" "$SECURITY_DIR/out/url-default.pdf"
grep -a -q "blocked-url" "$SECURITY_DIR/out/url-default.pdf"
"$BIN" --export "$SECURITY_DIR/doc/url.md" "$SECURITY_DIR/out/url-loopback.pdf" native elegant normal --allow-url-images
grep -a -q "blocked-url" "$SECURITY_DIR/out/url-loopback.pdf"

"$BIN" --export tester.md benchmark-output/linux-verify/tester.pdf native elegant normal
"$BIN" --bench docs/benchmark_smoke.md benchmark-output/linux-verify/bench-unicode 100 elegant normal

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

LINK_DIR=benchmark-output/linux-verify/links
mkdir -p "$LINK_DIR"

cat > "$LINK_DIR/ascii.md" <<'EOF'
# ASCII Link Regression

[one](https://example.com/one) with a long [wrapped destination](https://example.com/two?query=alpha) that must stay clickable after wrapping.
EOF

cat > "$LINK_DIR/unicode.md" <<'EOF'
# Unicode Link Regression

Za???? g??l? ja??: [unicode destination](https://example.com/unicode) and [second destination](https://example.com/second).
EOF

"$BIN" --export "$LINK_DIR/ascii.md" "$LINK_DIR/ascii.pdf" native tech margin=54pt
"$BIN" --export "$LINK_DIR/unicode.md" "$LINK_DIR/unicode.pdf" native modern normal
test -s "$LINK_DIR/ascii.pdf"
test -s "$LINK_DIR/unicode.pdf"
grep -a -q 'https://example.com/one' "$LINK_DIR/ascii.pdf"
grep -a -q 'https://example.com/two?query=alpha' "$LINK_DIR/ascii.pdf"
grep -a -q 'https://example.com/unicode' "$LINK_DIR/unicode.pdf"
link_count="$(grep -a -o '/Subtype /Link' "$LINK_DIR/ascii.pdf" "$LINK_DIR/unicode.pdf" | wc -l | tr -d ' ')"
if [ "$link_count" -lt 4 ]; then
    echo "Expected four link annotations across ASCII and Unicode exports." >&2
    exit 1
fi
grep -a -q '/Subtype /Link /Rect \[' "$LINK_DIR/ascii.pdf"
grep -a -q '/Subtype /Link /Rect \[' "$LINK_DIR/unicode.pdf"
echo
echo "No-network benchmark:"
cat benchmark-output/linux-verify/bench-unicode/bench-results.txt
echo
echo "ASCII benchmark:"
cat benchmark-output/linux-verify/bench-ascii/bench-results.txt
echo
echo "Linux verification complete."
