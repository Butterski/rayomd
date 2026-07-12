#!/usr/bin/env python3
"""Archive perf-watch records for published RayoMD release binaries.

This helper is intended to run on Linux or WSL. It downloads default
`linux-x64` release tarballs with `gh`, extracts the CLI binary under ignored
`benchmark-output/`, and records compact version benchmark JSON files through
`scripts/perf_watch.py`.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tarfile
from pathlib import Path
from typing import Any


DEFAULT_REPO = "Butterski/rayomd"
DEFAULT_ROOT = Path("benchmark-output/release-benchmark-archive")
DEFAULT_LOG_DIR = Path("docs/benchmarks/releases")
VERSION_RE = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)$")


def run(cmd: list[str], cwd: Path) -> str:
    proc = subprocess.run(cmd, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        raise RuntimeError(
            f"command failed rc={proc.returncode}: {' '.join(cmd)}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    return proc.stdout


def version_key(version: str) -> tuple[int, int, int]:
    match = VERSION_RE.match(version)
    if not match:
        raise ValueError(f"unsupported release tag: {version}")
    return tuple(int(part) for part in match.groups())


def normalize_version(version: str) -> str:
    return ".".join(str(part) for part in version_key(version))


def release_tags(repo: str, cwd: Path, from_version: str, to_version: str | None) -> list[str]:
    raw = run(
        [
            "gh",
            "release",
            "list",
            "-R",
            repo,
            "--limit",
            "100",
            "--json",
            "tagName,isDraft,isPrerelease",
        ],
        cwd,
    )
    minimum = version_key(from_version)
    maximum = version_key(to_version) if to_version else None
    tags: list[str] = []
    for item in json.loads(raw):
        if item.get("isDraft") or item.get("isPrerelease"):
            continue
        tag = str(item.get("tagName") or "")
        if not VERSION_RE.match(tag):
            continue
        key = version_key(tag)
        if key < minimum:
            continue
        if maximum is not None and key > maximum:
            continue
        tags.append("v" + normalize_version(tag))
    return sorted(set(tags), key=version_key)


def find_binary(extract_dir: Path) -> Path:
    matches = [path for path in extract_dir.rglob("rayomd") if path.is_file()]
    if not matches:
        raise RuntimeError(f"no rayomd binary found under {extract_dir}")
    return matches[0]


def safe_extract_tar(archive: Path, extract_dir: Path) -> None:
    destination = extract_dir.resolve()
    with tarfile.open(archive, "r:gz") as tar:
        for member in tar.getmembers():
            member_path = (extract_dir / member.name).resolve()
            if destination != member_path and destination not in member_path.parents:
                raise RuntimeError(f"archive member escapes destination: {member.name}")
        tar.extractall(extract_dir)


def existing_binary(binary_root: Path, tag: str) -> Path:
    return find_binary(binary_root / tag)


def download_and_extract(repo: str, tag: str, root: Path, cwd: Path) -> Path:
    version = normalize_version(tag)
    downloads = root / "downloads" / tag
    extract_dir = root / tag
    downloads.mkdir(parents=True, exist_ok=True)
    extract_dir.mkdir(parents=True, exist_ok=True)

    asset_name = f"rayomd-{version}-linux-x64.tar.gz"
    run(
        [
            "gh",
            "release",
            "download",
            tag,
            "-R",
            repo,
            "--pattern",
            asset_name,
            "--dir",
            str(downloads),
            "--clobber",
        ],
        cwd,
    )
    archive = downloads / asset_name
    if not archive.exists():
        raise RuntimeError(f"downloaded asset not found: {archive}")
    safe_extract_tar(archive, extract_dir)
    binary = find_binary(extract_dir)
    binary.chmod(binary.stat().st_mode | 0o111)
    return binary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=DEFAULT_REPO)
    parser.add_argument("--from-version", default="1.1.0")
    parser.add_argument("--to-version", default="")
    parser.add_argument("--versions", default="", help="comma-separated versions to archive, for example 1.1.0,1.1.1")
    parser.add_argument("--suite", choices=("quick", "watch", "full"), default="quick")
    parser.add_argument("--platform", default="linux-wsl-release")
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--binary-root", type=Path, default=None, help="existing extracted binary root used with --skip-download")
    parser.add_argument("--skip-download", action="store_true", help="use already extracted binaries under --binary-root instead of gh downloads")
    parser.add_argument("--version-log-dir", type=Path, default=DEFAULT_LOG_DIR)
    parser.add_argument(
        "--storage-note",
        default="WSL /mnt/e Windows-mounted storage; release linux-x64 tarball; quick suite",
    )
    parser.add_argument("--label", default="archive")
    args = parser.parse_args()

    cwd = Path.cwd()
    if args.versions:
        tags = ["v" + normalize_version(item.strip()) for item in args.versions.split(",") if item.strip()]
    else:
        tags = release_tags(args.repo, cwd, args.from_version, args.to_version or None)
    if not tags:
        print("No matching releases found.", file=sys.stderr)
        return 2
    if args.skip_download and args.binary_root is None:
        print("--skip-download requires --binary-root.", file=sys.stderr)
        return 2

    for tag in tags:
        version = normalize_version(tag)
        print(f"== {tag} ==", flush=True)
        binary = existing_binary(args.binary_root, tag) if args.skip_download else download_and_extract(args.repo, tag, args.root, cwd)
        run(
            [
                sys.executable,
                "-B",
                "scripts/perf_watch.py",
                "--binary",
                str(binary),
                "--platform",
                args.platform,
                "--suite",
                args.suite,
                "--label",
                args.label,
                "--root",
                str(args.root / "runs"),
                "--version-log-dir",
                str(args.version_log_dir),
                "--benchmark-version",
                version,
                "--storage-note",
                args.storage_note,
            ],
            cwd,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
