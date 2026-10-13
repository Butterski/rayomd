#!/usr/bin/env python3
"""Generate src/hot.ld, the GNU ld script that groups the code a fresh export runs.

A one-shot `rayomd --export` faults its code in by 64 KB windows of the executable. Release
builds put every function in a section of its own (-ffunction-sections); this script finds the
sections a set of documents executes and writes a linker script that places them, most used
first, in one output section before .text:

  SECTIONS { .text.hot : { *(.text._ZN...) ... *libc.a:malloc.o(.text) ... } } INSERT BEFORE .text;

CMake passes the script to GNU ld for Release builds on Linux (dynamic and RAYOMD_STATIC), where
the fresh exports of the documents took 3 to 5 fewer page faults. Placement changes no PDF byte.
A name that no longer exists matches nothing and is ignored, so a stale list is harmless; it
only stops helping. Regenerate it after larger changes to the code that every export runs:

  cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release && cmake --build build/linux
  cmake -S . -B build/static -DCMAKE_BUILD_TYPE=Release -DRAYOMD_STATIC=ON && cmake --build build/static
  python3 scripts/hot_sections.py --build-dir build/linux --build-dir build/static

For each build directory the script relinks rayomd from its objects without -s and without the
current hot.ld (CMakeFiles/rayomd.dir/link.txt), with a link map, runs `valgrind --tool=callgrind
--dump-instr=yes` on a fresh --export of every document, and maps each executed instruction to
the input section that holds it. Sections are ordered by how many documents ran them, then by
their place in the default layout. Archive members without function sections (glibc's in the
static build) and generic section names of single objects are named with their file.

Documents: the repository's README.md, tester.md and docs/development/*.md, plus a short ASCII
note and a short Polish note written by the script, or those given with --doc. Exports use the
font in RAYOMD_FONT, as usual. Needs valgrind and the compiler the build directory was configured
with; Python 3 standard library only.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]

NOTE = """# Meeting notes

Short notes with a [link](https://example.com), `code`, **bold** and *italic* text.

- First item
- Second item with a longer line of text that wraps once it reaches the margin of the page.

```sh
cmake --build build
```
"""

POLISH = """# Notatki ze spotkania

Zażółć gęślą jaźń. Pchnąć w tę łódź jeża lub ośm skrzyń fig.

- Pierwszy punkt
- Drugi punkt z dłuższym zdaniem, które zawija się na końcu wiersza.
"""

# Section names that more than one object uses: only with their file they name one section.
GENERIC = {".text", ".text.unlikely", ".text.hot", ".text.startup", ".text.exit"}


def parse_map(path: Path):
    """Input sections of the executable's code: (name, address, size, file)."""
    sections = []
    pending = None
    started = False
    for line in path.read_text(errors="replace").splitlines():
        if not started:
            started = line.startswith("Linker script and memory map")
            continue
        if pending is not None:
            m = re.match(r"\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$", line)
            if m:
                sections.append((pending, int(m.group(1), 16), int(m.group(2), 16), m.group(3).strip()))
            pending = None
            continue
        m = re.match(r" (\.text\S*)\s*$", line)
        if m:
            pending = m.group(1)
            continue
        m = re.match(r" (\.text\S*)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$", line)
        if m:
            sections.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4).strip()))
    return [s for s in sections if s[2] > 0]


def pattern(name: str, file: str) -> str:
    """The input section description of one section in a GNU ld script."""
    m = re.match(r"(.*/)?([^/]+\.a)\(([^)]+)\)$", file)
    if m:
        archive = re.sub(r"-\d+(\.\d+)*\.a$", "-*.a", m.group(2))   # libm-2.39.a: any glibc
        return f"*{archive}:{m.group(3)}({name})"
    if name in GENERIC:
        return f"*{os.path.basename(file)}({name})"
    return f"*({name})"


def executed_addresses(callgrind_out: Path, binary: Path):
    """The instruction addresses the profiled binary itself executed, and main's lowest one."""
    addresses = set()
    in_binary = False
    current_fn = ""
    main_low = None
    for line in callgrind_out.read_text(errors="replace").splitlines():
        if line.startswith("ob="):
            in_binary = Path(line[3:].strip()).name == binary.name
        elif line.startswith("fn="):
            current_fn = line[3:].strip()
        elif in_binary and line.startswith("0x"):
            address = int(line.split()[0], 16)
            addresses.add(address)
            if current_fn == "main" and (main_low is None or address < main_low):
                main_low = address
    return addresses, main_low


def symbol_address(binary: Path, name: str) -> int:
    out = subprocess.run(["nm", "--defined-only", str(binary)], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name and parts[1] in "Tt":
            return int(parts[0], 16)
    raise SystemExit(f"no symbol {name} in {binary}")


def relink(build: Path, work: Path) -> tuple[Path, Path]:
    link = (build / "CMakeFiles" / "rayomd.dir" / "link.txt").read_text().strip()
    args = [a for a in shlex.split(link) if a != "-s" and not re.match(r"-Wl,-T,.*hot\.ld$", a)]
    out = args.index("-o")
    binary, map_file = work / f"{build.name}-rayomd", work / f"{build.name}.map"
    args[out + 1] = str(binary)
    args.insert(1, f"-Wl,-Map={map_file}")
    subprocess.run(args, cwd=build, check=True)
    return binary, map_file


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--build-dir", action="append", required=True, type=Path,
                        help="a configured and built Release tree; repeat for the dynamic and the static build")
    parser.add_argument("--doc", action="append", type=Path, help="a Markdown document to export (repeatable)")
    parser.add_argument("--out", type=Path, default=ROOT / "src" / "hot.ld")
    parser.add_argument("--valgrind", default="valgrind")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="rayomd-hot-") as temp:
        work = Path(temp)
        docs = args.doc
        if not docs:
            (work / "notes.md").write_text(NOTE, encoding="utf-8")
            (work / "polish.md").write_text(POLISH, encoding="utf-8")
            docs = [work / "notes.md", work / "polish.md", ROOT / "README.md", ROOT / "tester.md"]
            docs += sorted((ROOT / "docs" / "development").glob("*.md"))
        uses: dict[str, int] = {}
        order: dict[str, tuple[int, int]] = {}
        for index, build in enumerate(args.build_dir):
            build = build.resolve()
            binary, map_file = relink(build, work)
            sections = sorted(parse_map(map_file), key=lambda s: s[1])
            starts = [s[1] for s in sections]
            main_link = symbol_address(binary, "main")
            for doc in docs:
                out = work / "cg.out"
                subprocess.run([args.valgrind, "--tool=callgrind", "--dump-instr=yes", "--compress-pos=no",
                                "--compress-strings=no", f"--callgrind-out-file={out}", str(binary), "--export",
                                str(doc.resolve()), str(work / "out.pdf"), "--no-image-cache"],
                               cwd=doc.resolve().parent, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                addresses, main_low = executed_addresses(out, binary)
                if main_low is None:
                    raise SystemExit(f"main not found in the profile of {doc}")
                bias = main_low - main_link   # where the loader put a PIE
                hit = set()
                for address in addresses:
                    address -= bias
                    lo, hi = 0, len(starts)
                    while lo < hi:
                        mid = (lo + hi) // 2
                        if starts[mid] <= address:
                            lo = mid + 1
                        else:
                            hi = mid
                    if lo and address < sections[lo - 1][1] + sections[lo - 1][2]:
                        hit.add(lo - 1)
                for at in hit:
                    name, address, _, file = sections[at]
                    key = pattern(name, file)
                    uses[key] = uses.get(key, 0) + 1
                    order.setdefault(key, (index, address))
                print(f"{build.name}: {doc.name}: {len(hit)} sections", file=sys.stderr)

    ranked = sorted(uses, key=lambda key: (-uses[key], order[key]))
    lines = [
        "/* The code fresh exports run, grouped before .text so that they fault in fewer pages.",
        "   Generated by scripts/hot_sections.py; regenerate it with that script, never by hand.",
        "   Names that no longer exist match nothing. */",
        "SECTIONS",
        "{",
        "  .text.hot :",
        "  {",
    ]
    lines += [f"    {key}" for key in ranked]
    lines += ["  }", "}", "INSERT BEFORE .text;", ""]
    args.out.write_text("\n".join(lines))
    print(f"{args.out}: {len(ranked)} sections", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
