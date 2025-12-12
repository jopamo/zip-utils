#!/usr/bin/env python3
"""
Document Info-ZIP 3.0 output and behavior across a curated option surface.

Linux only
Requires: zip, unzip
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from datetime import datetime
from itertools import count
from pathlib import Path
from typing import Callable


@dataclass
class CommandSpec:
    label: str
    argv: list[str]
    stdin: bytes | None = None
    before: Callable[[Path], None] | None = None


@dataclass
class CommandCapture:
    label: str
    argv: list[str]
    stdin_mode: str
    returncode: int
    stdout: str
    stderr: str
    duration_ms: int

    def to_dict(self) -> dict[str, object]:
        return {
            "label": self.label,
            "argv": self.argv,
            "stdin_mode": self.stdin_mode,
            "returncode": self.returncode,
            "stdout": self.stdout,
            "stderr": self.stderr,
            "duration_ms": self.duration_ms,
        }


@dataclass
class Scenario:
    name: str
    description: str
    workdir: Path
    archive: Path | None
    commands: list[CommandSpec]
    notes: list[str]
    capture_entries: bool = True


@dataclass
class ScenarioResult:
    name: str
    description: str
    workdir: str
    archive_path: str | None
    archive_exists: bool
    archive_size: int
    archive_sha256: str
    entries: list[str]
    commands: list[CommandCapture]
    notes: list[str]

    def to_dict(self) -> dict[str, object]:
        return {
            "name": self.name,
            "description": self.description,
            "workdir": self.workdir,
            "archive_path": self.archive_path,
            "archive_exists": self.archive_exists,
            "archive_size": self.archive_size,
            "archive_sha256": self.archive_sha256,
            "entries": self.entries,
            "commands": [c.to_dict() for c in self.commands],
            "notes": self.notes,
        }


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def run(argv: list[str], cwd: Path, stdin: bytes | None):
    start = datetime.now()
    proc = subprocess.run(
        argv,
        cwd=cwd,
        input=stdin,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    end = datetime.now()
    return (
        proc.returncode,
        proc.stdout.decode(errors="replace"),
        proc.stderr.decode(errors="replace"),
        int((end - start).total_seconds() * 1000),
    )


def capture_command(spec: CommandSpec, cwd: Path) -> CommandCapture:
    if spec.before:
        spec.before(cwd)
    rc, out, err, dur = run(spec.argv, cwd, spec.stdin)
    stdin_mode = "pipe" if spec.stdin is not None else "none"
    return CommandCapture(
        label=spec.label,
        argv=spec.argv,
        stdin_mode=stdin_mode,
        returncode=rc,
        stdout=out,
        stderr=err,
        duration_ms=dur,
    )


def list_entries(archive: Path) -> list[str]:
    if not shutil.which("unzip"):
        return []

    rc, out, _, _ = run(
        ["unzip", "-Z", "-1", str(archive)],
        archive.parent,
        None,
    )
    if rc != 0:
        return []

    return [line.strip() for line in out.splitlines() if line.strip()]


def make_fixture(root: Path) -> None:
    (root / "dir/sub").mkdir(parents=True)
    (root / "a.txt").write_text("hello\nworld\n")
    (root / "b.bin").write_bytes(os.urandom(256))
    (root / "crlf.txt").write_bytes(b"one\r\ntwo\r\n")
    (root / "dir/c.txt").write_text("inside\n")
    (root / "dir/sub/d.txt").write_text("nested\n")
    try:
        (root / "link").symlink_to("a.txt")
    except OSError:
        pass


def write_random_file(path: Path, size: int) -> None:
    remaining = size
    with path.open("wb") as f:
        while remaining > 0:
            chunk = min(remaining, 1024 * 1024)
            f.write(os.urandom(chunk))
            remaining -= chunk


def set_mtime(path: Path, dt: datetime) -> None:
    ts = dt.timestamp()
    os.utime(path, (ts, ts))


def build_workdir(fixture: Path, root: Path, name: str) -> Path:
    workdir = root / name
    shutil.copytree(fixture, workdir, symlinks=True)
    return workdir


def build_scenarios(zip_cmd: str, fixture: Path, root: Path) -> list[Scenario]:
    scenarios: list[Scenario] = []
    counter = count(1)

    def new_workdir(label: str) -> tuple[str, Path, Path, str]:
        name = f"{next(counter):02d}-{label}"
        workdir = build_workdir(fixture, root, name)
        archive = workdir / "out.zip"
        archive_name = "out.zip"
        return name, workdir, archive, archive_name

    name, workdir, archive, archive_name = new_workdir("create-default")
    scenarios.append(
        Scenario(
            name=name,
            description="Default add of fixture files.",
            workdir=workdir,
            archive=archive,
            commands=[CommandSpec("create", [zip_cmd, archive_name, "a.txt", "b.bin", "crlf.txt"])],
            notes=[],
        )
    )

    name, workdir, archive, archive_name = new_workdir("recurse-with-paths")
    scenarios.append(
        Scenario(
            name=name,
            description="Recursive add while preserving directory names.",
            workdir=workdir,
            archive=archive,
            commands=[CommandSpec("recursive", [zip_cmd, "-r", archive_name, "dir"])],
            notes=[],
        )
    )

    name, workdir, archive, archive_name = new_workdir("recurse-junk-paths")
    scenarios.append(
        Scenario(
            name=name,
            description="Recursive add while junking paths.",
            workdir=workdir,
            archive=archive,
            commands=[CommandSpec("recursive-junk", [zip_cmd, "-r", "-j", archive_name, "dir"])],
            notes=[],
        )
    )

    name, workdir, archive, archive_name = new_workdir("update-newer")
    scenarios.append(
        Scenario(
            name=name,
            description="Update entries when sources are newer (-u).",
            workdir=workdir,
            archive=archive,
            commands=[
                CommandSpec("seed", [zip_cmd, archive_name, "a.txt", "b.bin"]),
                CommandSpec(
                    "update",
                    [zip_cmd, "-u", archive_name, "a.txt"],
                    before=lambda _: (workdir / "a.txt").write_text("hello\nworld\nupdated\n"),
                ),
            ],
            notes=["Rewrites a.txt between seed and -u to trigger replacement."],
        )
    )

    name, workdir, archive, archive_name = new_workdir("freshen-existing")
    scenarios.append(
        Scenario(
            name=name,
            description="Freshen existing entries (-f) after touching source.",
            workdir=workdir,
            archive=archive,
            commands=[
                CommandSpec("seed", [zip_cmd, archive_name, "a.txt", "b.bin"]),
                CommandSpec(
                    "freshen",
                    [zip_cmd, "-f", archive_name, "a.txt"],
                    before=lambda _: (workdir / "a.txt").write_text("freshened\n"),
                ),
            ],
            notes=["a.txt edited before the -f run to exercise freshen semantics."],
        )
    )

    name, workdir, archive, archive_name = new_workdir("delete-entry")
    scenarios.append(
        Scenario(
            name=name,
            description="Delete a single entry (-d) from an existing archive.",
            workdir=workdir,
            archive=archive,
            commands=[
                CommandSpec("seed", [zip_cmd, archive_name, "a.txt", "dir/c.txt", "dir/sub/d.txt"]),
                CommandSpec("delete", [zip_cmd, "-d", archive_name, "dir/c.txt"]),
            ],
            notes=["Removes dir/c.txt after seeding the archive with multiple entries."],
        )
    )

    name, workdir, archive, archive_name = new_workdir("filesync-prune-missing")
    orphan = workdir / "orphan.txt"
    orphan.write_text("stale\n")
    scenarios.append(
        Scenario(
            name=name,
            description="File sync (-FS) removes entries missing from disk.",
            workdir=workdir,
            archive=archive,
            commands=[
                CommandSpec("seed", [zip_cmd, archive_name, "a.txt", "orphan.txt"]),
                CommandSpec(
                    "filesync",
                    [zip_cmd, "-FS", archive_name, "a.txt", "orphan.txt"],
                    before=lambda _: orphan.unlink(missing_ok=True),
                ),
            ],
            notes=["Deletes orphan.txt from the filesystem before running -FS."],
        )
    )

    name, workdir, archive, archive_name = new_workdir("move-sources")
    scenarios.append(
        Scenario(
            name=name,
            description="Move inputs into the archive (-m).",
            workdir=workdir,
            archive=archive,
            commands=[CommandSpec("move", [zip_cmd, "-m", archive_name, "a.txt", "dir/c.txt"])],
            notes=["Sources should be removed after a successful write."],
        )
    )

    name, workdir, archive, archive_name = new_workdir("encrypt-password")
    scenarios.append(
        Scenario(
            name=name,
            description="Password-protected archive via -P (ZipCrypto).",
            workdir=workdir,
            archive=archive,
            commands=[CommandSpec("encrypt", [zip_cmd, "-P", "s3cr3t", archive_name, "b.bin"])],
            notes=[],
        )
    )

    name, workdir, archive, archive_name = new_workdir("split-archive")
    big = workdir / "big.bin"
    scenarios.append(
        Scenario(
            name=name,
            description="Split output with -s using a multi-megabyte input.",
            workdir=workdir,
            archive=archive,
            commands=[
                CommandSpec(
                    "split",
                    [zip_cmd, "-s", "200k", archive_name, big.name],
                    before=lambda _: write_random_file(big, 3 * 1024 * 1024),
                )
            ],
            notes=["big.bin (~3 MiB) created to force multiple parts."],
        )
    )

    name, workdir, archive, archive_name = new_workdir("stdin-names")
    scenarios.append(
        Scenario(
            name=name,
            description="Names read from stdin with -@.",
            workdir=workdir,
            archive=archive,
            commands=[
                CommandSpec(
                    "stdin-names",
                    [zip_cmd, "-@", archive_name],
                    stdin=b"a.txt\nb.bin\n",
                )
            ],
            notes=[],
        )
    )

    name, workdir, archive, archive_name = new_workdir("test-after-write")
    scenarios.append(
        Scenario(
            name=name,
            description="Test archive contents after writing (-T).",
            workdir=workdir,
            archive=archive,
            commands=[CommandSpec("test-after-write", [zip_cmd, "-T", archive_name, "a.txt", "b.bin"])],
            notes=[],
        )
    )

    name, workdir, archive, archive_name = new_workdir("time-filter")
    scenarios.append(
        Scenario(
            name=name,
            description="Date filter (-t) drops older sources.",
            workdir=workdir,
            archive=archive,
            commands=[
                CommandSpec(
                    "filter-after-date",
                    [zip_cmd, "-t", "2020-01-01", archive_name, "a.txt", "dir/c.txt"],
                    before=lambda _: set_mtime(workdir / "a.txt", datetime(2010, 1, 1)),
                )
            ],
            notes=["Backdates a.txt to 2010 before applying the cutoff date."],
        )
    )

    name, workdir, _, archive_name = new_workdir("stdin-filter-mode")
    scenarios.append(
        Scenario(
            name=name,
            description="Bare invocation reading stdin (usage behavior).",
            workdir=workdir,
            archive=None,
            commands=[CommandSpec("stdin-filter", [zip_cmd], stdin=b"stdin\nfilter\n")],
            notes=["Captures return code/output when zip is invoked without args but with stdin data."],
            capture_entries=False,
        )
    )

    return scenarios


def execute_scenario(scenario: Scenario) -> ScenarioResult:
    captures: list[CommandCapture] = []
    for spec in scenario.commands:
        captures.append(capture_command(spec, scenario.workdir))

    archive_exists = scenario.archive is not None and scenario.archive.exists()
    archive_size = scenario.archive.stat().st_size if archive_exists else 0
    archive_sha = sha256_file(scenario.archive) if archive_exists else ""
    entries: list[str] = []
    if scenario.capture_entries and archive_exists:
        entries = list_entries(scenario.archive)

    return ScenarioResult(
        name=scenario.name,
        description=scenario.description,
        workdir=str(scenario.workdir),
        archive_path=str(scenario.archive) if scenario.archive else None,
        archive_exists=archive_exists,
        archive_size=archive_size,
        archive_sha256=archive_sha,
        entries=entries,
        commands=captures,
        notes=scenario.notes,
    )


def write_outputs(outdir: Path, results: list[ScenarioResult], metadata: dict[str, object]) -> None:
    with (outdir / "runs.jsonl").open("w") as f:
        for res in results:
            f.write(json.dumps(res.to_dict()) + "\n")

    with (outdir / "metadata.json").open("w") as f:
        json.dump(metadata, f, indent=2)

    by_rc: dict[int, int] = {}
    for res in results:
        rc = res.commands[-1].returncode if res.commands else -1
        by_rc[rc] = by_rc.get(rc, 0) + 1

    summary = outdir / "summary.md"
    with summary.open("w") as f:
        f.write("# zip output behavior summary\n\n")
        f.write(f"- zip command: {metadata.get('zip_cmd')}\n")
        f.write(f"- zip version: {metadata.get('zip_version')}\n")
        f.write(f"- scenarios: {len(results)}\n")
        for rc in sorted(by_rc):
            f.write(f"- rc={rc}: {by_rc[rc]} scenario(s)\n")
        f.write("\n## Scenarios\n")
        for res in results:
            main_rc = res.commands[-1].returncode if res.commands else -1
            entry_count = len(res.entries)
            archive_state = "archive" if res.archive_exists else "no archive"
            f.write(
                f"- {res.name} (rc={main_rc}, {archive_state}, entries={entry_count}): {res.description}\n"
            )
            if res.notes:
                for note in res.notes:
                    f.write(f"  - note: {note}\n")


def get_zip_version(zip_cmd: str) -> str:
    proc = subprocess.run(
        [zip_cmd, "-v"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if proc.stdout:
        first = proc.stdout.splitlines()[0].strip()
        if first:
            return first
    if proc.stderr:
        return proc.stderr.splitlines()[0].strip()
    return f"rc={proc.returncode}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--zip", default="zip", help="Path to the upstream zip binary to document.")
    ap.add_argument("--outdir", default="zip-doc", help="Output directory for captures.")
    ap.add_argument(
        "--max-runs",
        type=int,
        default=0,
        help="Limit number of scenarios to run (0 = all).",
    )
    args = ap.parse_args()

    if not shutil.which(args.zip):
        print(f"zip binary not found: {args.zip}", file=sys.stderr)
        return 1

    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    zip_version = get_zip_version(args.zip)

    results: list[ScenarioResult] = []
    with tempfile.TemporaryDirectory(prefix="zip-doc-") as td:
        tmp_root = Path(td)
        fixture = tmp_root / "fixture"
        fixture.mkdir()
        make_fixture(fixture)

        scenarios = build_scenarios(args.zip, fixture, tmp_root)
        limit = args.max_runs if args.max_runs > 0 else len(scenarios)
        for scenario in scenarios[:limit]:
            results.append(execute_scenario(scenario))

    metadata = {
        "zip_cmd": args.zip,
        "zip_version": zip_version,
        "generated_at": datetime.now().isoformat(),
    }
    write_outputs(outdir, results, metadata)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
