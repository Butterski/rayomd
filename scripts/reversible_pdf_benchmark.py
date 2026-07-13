#!/usr/bin/env python3
"""Benchmark and validate the rayomd-source/1 reversible PDF profile."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import time
import zlib

try:
    import psutil  # type: ignore
except ImportError:  # pragma: no cover - optional local measurement helper
    psutil = None


ROOT = Path(__file__).resolve().parents[1]
SOURCE_LIMIT = 10 * 1024 * 1024
PDF_LIMIT = 256 * 1024 * 1024
PAYLOAD = (
    b"# Heading\n\nParagraph with **bold**, [link](https://example.com), and text.\n\n"
    b"| Left | Right |\n|---|---:|\n| alpha | 42 |\n\n"
)


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def exact_payload(size: int) -> bytes:
    if size == 0:
        return b""
    repeated, remainder = divmod(size, len(PAYLOAD))
    return PAYLOAD * repeated + PAYLOAD[:remainder]


def measured(
    command: list[str],
    expected: tuple[int, ...] = (0,),
    env: dict[str, str] | None = None,
) -> dict[str, object]:
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) if os.name == "nt" else 0
    started = time.perf_counter()
    process = subprocess.Popen(
        command,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        creationflags=flags,
        env=env,
    )
    peak_rss = 0
    observed = psutil.Process(process.pid) if psutil is not None else None
    while process.poll() is None:
        if observed is not None:
            try:
                current_rss = observed.memory_info().rss
                for child in observed.children(recursive=True):
                    current_rss += child.memory_info().rss
                peak_rss = max(peak_rss, current_rss)
            except (psutil.Error, OSError):
                pass
        time.sleep(0.005)
    stdout, stderr = process.communicate()
    wall_ms = (time.perf_counter() - started) * 1000.0
    if process.returncode not in expected:
        raise RuntimeError(
            f"command failed ({process.returncode}): {' '.join(command)}\n"
            f"stdout: {stdout.decode(errors='replace')}\n"
            f"stderr: {stderr.decode(errors='replace')}"
        )
    return {
        "exit_code": process.returncode,
        "wall_ms": wall_ms,
        "peak_rss_bytes": peak_rss or None,
        "stdout": stdout.decode(errors="replace"),
        "stderr": stderr.decode(errors="replace"),
    }


def summarize(samples: list[dict[str, object]]) -> dict[str, object]:
    walls = [float(sample["wall_ms"]) for sample in samples]
    rss = [int(sample["peak_rss_bytes"]) for sample in samples if sample["peak_rss_bytes"]]
    return {
        "samples": len(samples),
        "wall_ms_p50": statistics.median(walls),
        "wall_ms_p95": percentile(walls, 0.95),
        "peak_rss_bytes": max(rss) if rss else None,
    }


def allocation_profile(binary: Path, command: list[str]) -> dict[str, int]:
    environment = os.environ.copy()
    environment["RAYOMD_PROFILE"] = "1"
    sample = measured([str(binary), *command], env=environment)
    match = re.search(
        r"RAYOMD_PROFILE label=build .*?allocations=(\d+) allocated_bytes=(\d+)",
        str(sample["stderr"]),
    )
    if not match:
        raise RuntimeError(f"profiling build emitted no build allocation record: {sample['stderr']}")
    return {"allocations": int(match.group(1)), "allocated_bytes": int(match.group(2))}


def pdf_literal(value: str) -> str:
    return value.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")


def xmp(source: bytes) -> bytes:
    digest = hashlib.sha256(source).hexdigest()
    return (
        '<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?>'
        '<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF '
        'xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"><rdf:Description '
        'xmlns:rayomd="https://rayomd.dev/ns/source/1.0/" '
        'rayomd:profile="rayomd-source/1" rayomd:producer="benchmark-fixture" '
        f'rayomd:encoding="UTF-8" rayomd:length="{len(source)}" '
        f'rayomd:sha256="{digest}" rayomd:attachment="source.md"/>'
        '</rdf:RDF></x:xmpmeta><?xpacket end="w"?>'
    ).encode("utf-8")


def profile_fixture(source: bytes, compressed: bool) -> bytes:
    encoded = zlib.compress(source, 6) if compressed else source
    metadata = xmp(source)
    page_content = b"q 0.15 0.55 0.85 rg 72 600 468 100 re f Q"
    filter_entry = " /Filter /FlateDecode" if compressed else ""
    objects: list[bytes] = [
        b"<< /Type /Catalog /Pages 2 0 R /Metadata 7 0 R "
        b"/Names << /EmbeddedFiles << /Names [(source.md) 6 0 R] >> >> /AF [6 0 R] >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << >> /Contents 4 0 R >>",
        f"<< /Length {len(page_content)} >>\nstream\n".encode("ascii") + page_content + b"\nendstream",
        (
            f"<< /Type /EmbeddedFile /Subtype /text#2Fmarkdown /Params << /Size {len(source)} >> "
            f"/Length {len(encoded)}{filter_entry} >>\nstream\n"
        ).encode("ascii") + encoded + b"\nendstream",
        b"<< /Type /Filespec /F (source.md) /UF (source.md) /EF << /F 5 0 R /UF 5 0 R >> /AFRelationship /Source >>",
        f"<< /Type /Metadata /Subtype /XML /Length {len(metadata)} >>\nstream\n".encode("ascii")
        + metadata + b"\nendstream",
    ]
    pdf = bytearray(b"%PDF-2.0\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for object_id, body in enumerate(objects, 1):
        offsets.append(len(pdf))
        pdf.extend(f"{object_id} 0 obj\n".encode("ascii"))
        pdf.extend(body)
        pdf.extend(b"\nendobj\n")
    xref_offset = len(pdf)
    pdf.extend(f"xref\n0 {len(objects) + 1}\n0000000000 65535 f \n".encode("ascii"))
    for offset in offsets[1:]:
        pdf.extend(f"{offset:010d} 00000 n \n".encode("ascii"))
    pdf.extend(
        f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref_offset}\n%%EOF\n".encode("ascii")
    )
    return bytes(pdf)


def validator(command: list[str]) -> dict[str, object]:
    try:
        result = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"available": False, "ok": False, "detail": str(error)}
    return {
        "available": True,
        "ok": result.returncode == 0,
        "detail": (result.stdout + result.stderr).decode(errors="replace")[-2000:],
    }


def run(args: argparse.Namespace) -> Path:
    binary = Path(args.binary).resolve()
    if not binary.is_file():
        raise FileNotFoundError(binary)
    profile_binary = Path(args.profile_binary).resolve() if args.profile_binary else None
    if profile_binary is not None and not profile_binary.is_file():
        raise FileNotFoundError(profile_binary)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    output = Path(args.root) / args.platform / f"{stamp}-{args.suite}"
    payload_dir = output / "payloads"
    payload_dir.mkdir(parents=True)
    sizes = [("empty", 0), ("10k", 10 * 1024), ("1m", 1024 * 1024)]
    if args.suite == "full":
        sizes.append(("10m-maximum", SOURCE_LIMIT))

    record: dict[str, object] = {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "platform": args.platform,
        "suite": args.suite,
        "binary": str(binary),
        "binary_bytes": binary.stat().st_size,
        "profile_binary": str(profile_binary) if profile_binary else None,
        "source_limit_bytes": SOURCE_LIMIT,
        "pdf_limit_bytes": PDF_LIMIT,
        "psutil_rss_available": psutil is not None,
        "cases": [],
    }
    case_records: list[dict[str, object]] = []
    for name, size in sizes:
        source = exact_payload(size)
        source_path = payload_dir / f"{name}.md"
        source_path.write_bytes(source)
        plain_pdf = output / f"{name}-plain.pdf"
        embedded_pdf = output / f"{name}-embedded.pdf"
        sample_count = args.samples if size <= 1024 * 1024 else min(args.samples, 3)

        plain_samples = [measured([str(binary), "--export", str(source_path), str(plain_pdf), "native", "modern", "normal"])
                         for _ in range(sample_count)]
        embedded_samples: list[dict[str, object]] = []
        embedded_exit = 0
        for _ in range(sample_count):
            sample = measured(
                [str(binary), "--export", str(source_path), str(embedded_pdf), "native", "modern", "normal", "--embed-source"],
                expected=(0, 16),
            )
            embedded_samples.append(sample)
            embedded_exit = int(sample["exit_code"])
            if embedded_exit != 0:
                break

        item: dict[str, object] = {
            "name": name,
            "source_bytes": size,
            "plain": summarize(plain_samples),
            "plain_pdf_bytes": plain_pdf.stat().st_size,
            "embedded_exit_code": embedded_exit,
            "embedded": summarize(embedded_samples),
        }
        if profile_binary is not None and embedded_exit == 0:
            profile_plain = output / f"{name}-profile-plain.pdf"
            profile_embedded = output / f"{name}-profile-embedded.pdf"
            item["allocation_profile"] = {
                "plain": allocation_profile(profile_binary, [
                    "--export", str(source_path), str(profile_plain), "native", "modern", "normal",
                ]),
                "embedded": allocation_profile(profile_binary, [
                    "--export", str(source_path), str(profile_embedded), "native", "modern", "normal",
                    "--embed-source",
                ]),
            }
        if embedded_exit == 0:
            item["embedded_pdf_bytes"] = embedded_pdf.stat().st_size
            inspect_samples = [measured([str(binary), "--inspect-source", str(embedded_pdf)])
                               for _ in range(sample_count)]
            recovery_samples: list[dict[str, object]] = []
            for sample_index in range(sample_count):
                recovered = output / f"{name}-recovered-{sample_index}.md"
                recovery_samples.append(measured([str(binary), "--recover-source", str(embedded_pdf), str(recovered)]))
                if recovered.read_bytes() != source:
                    raise AssertionError(f"{name}: recovered bytes differ")
                recovered.unlink()
            item["inspect"] = summarize(inspect_samples)
            item["recover"] = summarize(recovery_samples)
        case_records.append(item)
    record["cases"] = case_records

    storage_source = exact_payload(1024 * 1024)
    raw_fixture = output / "storage-raw.pdf"
    compressed_fixture = output / "storage-flate.pdf"
    raw_fixture.write_bytes(profile_fixture(storage_source, False))
    compressed_fixture.write_bytes(profile_fixture(storage_source, True))
    storage: dict[str, object] = {
        "source_bytes": len(storage_source),
        "raw_pdf_bytes": raw_fixture.stat().st_size,
        "flate_pdf_bytes": compressed_fixture.stat().st_size,
        "flate_payload_bytes": len(zlib.compress(storage_source, 6)),
    }
    qpdf = shutil.which("qpdf")
    pdfinfo = shutil.which("pdfinfo")
    pdfdetach = shutil.which("pdfdetach")
    storage["qpdf_raw"] = validator([qpdf, "--check", str(raw_fixture)]) if qpdf else {"available": False}
    storage["qpdf_flate"] = validator([qpdf, "--check", str(compressed_fixture)]) if qpdf else {"available": False}
    storage["pdfinfo_raw"] = validator([pdfinfo, str(raw_fixture)]) if pdfinfo else {"available": False}
    storage["pdfinfo_flate"] = validator([pdfinfo, str(compressed_fixture)]) if pdfinfo else {"available": False}
    storage["pdfdetach_available"] = bool(pdfdetach)
    if pdfdetach:
        for label, fixture in (("raw", raw_fixture), ("flate", compressed_fixture)):
            detached = output / f"detached-{label}.md"
            check = validator([pdfdetach, "-save", "1", "-o", str(detached), str(fixture)])
            check["exact_source"] = bool(check.get("ok")) and detached.is_file() and detached.read_bytes() == storage_source
            storage[f"pdfdetach_{label}"] = check
    storage["product_raw_inspect"] = measured([str(binary), "--inspect-source", str(raw_fixture)])["exit_code"]
    storage["product_flate_inspect"] = measured([str(binary), "--inspect-source", str(compressed_fixture)], expected=(32,))["exit_code"]
    record["storage_experiment"] = storage

    record_path = output / "record.json"
    record_path.write_text(json.dumps(record, indent=2), encoding="utf-8")
    lines = [
        "# Reversible PDF benchmark",
        "",
        f"Platform: `{args.platform}`  ",
        f"Suite: `{args.suite}`  ",
        f"Binary: `{binary}` ({binary.stat().st_size:,} bytes)",
        "",
        "| Case | Source | Plain PDF | Embedded PDF | Plain p50/p95 | Embedded p50/p95 | Inspect p50/p95 | Recover p50/p95 | Peak RSS |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for item in case_records:
        embedded_size = item.get("embedded_pdf_bytes")
        embedded = item["embedded"]
        inspect = item.get("inspect", {})
        recover = item.get("recover", {})
        peak = max(
            int(item["plain"].get("peak_rss_bytes") or 0),
            int(embedded.get("peak_rss_bytes") or 0),
            int(inspect.get("peak_rss_bytes") or 0),
            int(recover.get("peak_rss_bytes") or 0),
        )
        lines.append(
            f"| {item['name']} | {int(item['source_bytes']):,} | {int(item['plain_pdf_bytes']):,} | "
            f"{f'{int(embedded_size):,}' if embedded_size is not None else 'rejected (limit)'} | "
            f"{float(item['plain']['wall_ms_p50']):.2f}/{float(item['plain']['wall_ms_p95']):.2f} ms | "
            f"{float(embedded['wall_ms_p50']):.2f}/{float(embedded['wall_ms_p95']):.2f} ms | "
            f"{f'{float(inspect['wall_ms_p50']):.2f}/{float(inspect['wall_ms_p95']):.2f} ms' if inspect else '-'} | "
            f"{f'{float(recover['wall_ms_p50']):.2f}/{float(recover['wall_ms_p95']):.2f} ms' if recover else '-'} | "
            f"{f'{peak / (1024 * 1024):.1f} MiB' if peak else 'unavailable'} |"
        )
    lines.extend((
        "",
        "## Storage experiment",
        "",
        f"- Raw fixture: {raw_fixture.stat().st_size:,} bytes",
        f"- Flate fixture: {compressed_fixture.stat().st_size:,} bytes",
        f"- Compressed source stream: {int(storage['flate_payload_bytes']):,} bytes",
        f"- qpdf available: {bool(qpdf)}; Poppler pdfinfo available: {bool(pdfinfo)}; pdfdetach available: {bool(pdfdetach)}",
        "- Product policy: raw fixture accepted; Flate fixture rejected by profile v1.",
        "",
        "Raw measurements: `record.json`.",
    ))
    if profile_binary is not None:
        lines.extend((
            "",
            "## Allocation delta",
            "",
            "| Case | Extra allocations | Extra allocated bytes |",
            "|---|---:|---:|",
        ))
        for item in case_records:
            profile = item.get("allocation_profile")
            if not profile:
                continue
            plain = profile["plain"]
            embedded = profile["embedded"]
            lines.append(
                f"| {item['name']} | {int(embedded['allocations']) - int(plain['allocations']):+,} | "
                f"{int(embedded['allocated_bytes']) - int(plain['allocated_bytes']):+,} |"
            )
    (output / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--suite", choices=("quick", "full"), default="quick")
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument(
        "--profile-binary",
        help="matching RAYOMD_ENABLE_PROFILING build used to record allocation deltas",
    )
    parser.add_argument("--root", default="benchmark-output/reversible-profile")
    args = parser.parse_args()
    if args.samples < 1 or args.samples > 20:
        parser.error("--samples must be between 1 and 20")
    output = run(args)
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
