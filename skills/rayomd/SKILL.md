---
name: rayomd
description: Converts Markdown to PDF with the rayomd command-line tool (one small binary, no browser, TeX or Pandoc), with formulas, Mermaid diagrams, highlighted code, tables, tables of contents, page numbers, books of several files, PDF/A and PDF/UA. Use when the user wants a PDF of Markdown the agent wrote or was given (a report, summary, release notes, documentation, a note or a folder of notes).
license: Apache-2.0
compatibility: Needs a shell and the rayomd binary (Linux x64 or Windows x64 release, or the ghcr.io/butterski/rayomd container); network access only to download it.
metadata:
  source: https://github.com/Butterski/rayomd
---

# Markdown to PDF with RayoMD

RayoMD reads Markdown and writes the PDF itself: one export of a short note takes about a
millisecond, the same input and flags give the same PDF bytes, and a document never fails
over a missing image, an unknown TeX command or an unsupported diagram (each degrades to
readable text). Before writing the Markdown, read [references/syntax.md](references/syntax.md)
for what renders and what degrades.

## 1. Get the binary

```sh
rayomd --version          # prints "rayomd 5.3.0"; on Windows: rayomd.exe --version
```

If it is not on `PATH`, download the release asset for the platform from
https://github.com/Butterski/rayomd/releases and check it against the SHA-256 that GitHub
lists for that asset (`digest` in the releases API, shown on the release page).

| Asset | Platform |
|---|---|
| `rayomd-<version>-linux-x64-static.tar.gz` | Linux x64, no dependency (use this one) |
| `rayomd-<version>-linux-x64-curl.tar.gz` | Linux x64 with glibc and libcurl: HTTP/HTTPS images |
| `rayomd-<version>-linux-x64.tar.gz` | Linux x64 with glibc |
| `rayomd-<version>-windows-x64.zip` | Windows x64, `rayomd.exe` |

Linux (needs `curl`, `python3`, `sha256sum`):

```sh
api=$(curl -fsSL https://api.github.com/repos/Butterski/rayomd/releases/latest)
set -- $(printf '%s' "$api" | python3 -c 'import json,sys
d=json.load(sys.stdin); v=d["tag_name"][1:]; n="rayomd-%s-linux-x64-static.tar.gz" % v
print(v, n, next(a["digest"] for a in d["assets"] if a["name"] == n).split(":")[1])')
v=$1 asset=$2 sum=$3
curl -fLO "https://github.com/Butterski/rayomd/releases/download/v$v/$asset"
echo "$sum  $asset" | sha256sum -c -        # stop if this does not print "OK"
tar -xzf "$asset"
export PATH="$PWD/rayomd-$v-linux-x64-static:$PATH"
rayomd --version
```

Windows (PowerShell):

```powershell
$r = Invoke-RestMethod https://api.github.com/repos/Butterski/rayomd/releases/latest
$v = $r.tag_name.TrimStart('v'); $asset = "rayomd-$v-windows-x64.zip"
$sum = ($r.assets | Where-Object name -eq $asset).digest -replace '^sha256:', ''
Invoke-WebRequest "https://github.com/Butterski/rayomd/releases/download/v$v/$asset" -OutFile $asset
if ((Get-FileHash $asset -Algorithm SHA256).Hash -ne $sum) { throw "SHA-256 mismatch: $asset" }
Expand-Archive $asset -DestinationPath .
$env:PATH = "$PWD\rayomd-$v-windows-x64;$env:PATH"
rayomd.exe --version
```

With Docker and no binary, the container image `ghcr.io/butterski/rayomd` (the static binary,
DejaVu Sans and nothing else; published from the first release whose release notes name it)
takes the same arguments:

```sh
docker run --rm -v "$PWD:/w" -w /w ghcr.io/butterski/rayomd --export report.md report.pdf
```

Paths are inside the container, so keep the Markdown, its images and the PDF under the mounted
folder. If the PDF cannot be written (exit code 12), add `-u "$(id -u):$(id -g)"`. Tags are
`latest` and each version (`ghcr.io/butterski/rayomd:<version>`).

`rayomd --doctor` checks fonts and a test export when something looks wrong.

## 2. Export

Write the Markdown to a file first and export the file: relative images resolve from the
Markdown file's folder.

```sh
rayomd --export report.md report.pdf
```

The command prints nothing on success and exits 0. `.pdf` is appended to an output name
without it.

Markdown already in a pipe (no images: with `--stdin` there is no source file to resolve them
from):

```sh
printf '%s\n' "$markdown" | rayomd --stdin report.pdf
```

Several files as one PDF (each from a new page, links between them kept, one outline):

```sh
rayomd --book docs/SUMMARY.md manual.pdf --toc     # files a SUMMARY.md lists (mdBook, GitBook)
rayomd --book chapters manual.pdf --toc            # a folder's .md files by name, README.md first
rayomd --book intro.md usage.md faq.md manual.pdf  # the files given, in that order
```

Options go after the output path, in any order. Style and margin are bare words:

| Ask | Arguments |
|---|---|
| Look | `elegant` (default), `modern`, `tech` |
| Margins | `compact`, `normal` (default), `wide`, `margin=0.75in`, `margin=54pt` |
| Table of contents | `--toc`, `--toc-depth=N` (1-6, default 3); or a `[TOC]` paragraph in the text |
| Numbered headings | `--number-headings` (3 levels), `--number-headings=N` (1-6) |
| Page numbers | `--page-numbers` ("N / M" at the foot of each page) |
| Paper | `--page-size=a4` (default), `a3`, `a5`, `letter`, `legal`, each also `-landscape` (`letter-landscape`), or `WIDTHxHEIGHT` in `mm`, `cm`, `in`, `pt` (`150mmx200mm`) |
| One long page for screens | `--page-size=210mmxauto` |
| Chapter per page | `--page-break-before=h1` (`h2`: sections too) |
| Footnotes at the foot of the page | `--footnotes=page` (default: after the text) |
| Archive | `--pdfa` (PDF/A-3b) |
| Accessible, screen readers | `--pdfua` (tagged PDF/UA-1; formulas show as TeX source); `lang:` in the front matter sets the language |
| Black-and-white printer | `--black-text` |
| Smaller file | `--compress` |
| Obsidian vault note | `--dialect=obsidian`, `--vault=DIR`, `--hide-tags` |
| Code in one colour / Mermaid as code | `--no-highlight` / `--no-diagrams` |
| Images outside the Markdown's folder | `--allow-unsafe-local-images` |
| HTTP/HTTPS images | `--allow-url-images` (Windows, and the Linux `-curl` build) |
| Company look (font, colours, header, footer, cover) | `--theme=FILE` |

Examples:

```sh
rayomd --export handbook.md handbook.pdf modern --toc --toc-depth=2 --number-headings --page-numbers
rayomd --export report.md report.pdf --page-size=letter-landscape wide
rayomd --export thesis.md thesis.pdf --footnotes=page --page-break-before=h1
rayomd --export policy.md policy.pdf --pdfa --pdfua --compress
rayomd --export report.md report.pdf --black-text --page-numbers
```

YAML front matter (`title`, `author`, `subject`, `keywords`, `lang`) fills the PDF's metadata and
is not printed.

### Many documents

For a folder: `rayomd --batch <in-folder> <out-folder> [flags] --report=-` (`--recursive`,
`--skip-unchanged`).

For many exports from one process, `rayomd --serve --json [flags]` reads one JSON request a
line on stdin and answers one JSON line a request, in order, with the fields of the report
below and the request's `id`. It comes with 5.3.0 (5.2.5 and older take `--json` for an output
folder and answer otherwise): its first output line is `{"rayomd":"<version>","protocol":1}`;
use it only when that line comes.

```sh
rayomd --serve --json modern --page-numbers < requests.jsonl
```

Request lines (`input` a Markdown file, or `markdown` text with an optional `source_path`
that its relative images resolve from; `output` required; `args` flags added to the command
line's):

```json
{"id":1,"input":"a.md","output":"a.pdf"}
{"id":2,"markdown":"# Hi\n\n$x^2$","source_path":"notes/hi.md","output":"hi.pdf","args":["--toc"]}
```

A line `quit` or the end of input ends the session.

## 3. Check the result

The exit code says whether a PDF was written: 0 written; 2 bad arguments; 3 input not readable;
10 and above a build error; 12 output not writable. A written PDF can still have degraded
parts. A single `--export` reports only missing characters, as a `Warning:` line on stderr;
for the counts, convert the file through a batch mode with `--report=-`, which prints one JSON
line a document on stdout:

```sh
echo report.md | rayomd --stdin-batch out --report=- --toc --page-numbers
```

```json
{"input":"report.md","output":"out/report.pdf","status":"ok","code":0,"pages":3,"bytes":41234,"ms":0.412,"missing_characters":0,"failed_images":0}
```

`out` is created if needed; the PDF there, `out/<name>.pdf`, is the one `--export` with the
same flags writes. A one-request `--serve --json` session (see above for which binaries have
it) gives the same record for any output path:

```sh
printf '%s\n' '{"input":"report.md","output":"report.pdf","args":["--toc","--page-numbers"]}' | rayomd --serve --json
```

Check `status` is `"ok"` and `pages` is plausible, then:

- `missing_characters` > 0: characters no font found has a glyph for, printed as a base letter
  or `?`. Install a font that has them (DejaVu Sans or Noto Sans for most alphabets, Noto Sans
  CJK or WenQuanYi Micro Hei for Chinese, Japanese and Korean), or set `RAYOMD_FONT` (the text
  font) or `RAYOMD_FALLBACK_FONT` (taken when the text font lacks characters; the document is
  then drawn in it, so it needs Latin too) to a `.ttf`, `.otf` or `.ttc` file, then export
  again. Or replace emoji and symbols no font has.
- `failed_images` > 0: images shown as their alt text. A relative path resolves from the
  Markdown file's folder and must stay inside it unless `--allow-unsafe-local-images`; check
  the path and the file's format (PNG, JPEG, SVG). URL images need `--allow-url-images` and a
  build that loads them (Windows, or the Linux `-curl` asset); otherwise download the image
  next to the Markdown and link the local file.
- `status` `"error"`: `error` says why; fix the input or the flags.

## 4. Preview in RayoMD GUI

When the user has [RayoMD GUI](https://github.com/Butterski/rayomd-gui) (the desktop editor
for Linux and Windows) and wants to watch the document:

```sh
rayomd-gui report.md
```

It opens the note beside a live PDF preview. Releases after 1.4.0 reload the file when the
agent rewrites it on disk and the editor holds no unsaved edits, so the preview follows each
rewrite; the PDF to hand over is still made with `rayomd --export`.
