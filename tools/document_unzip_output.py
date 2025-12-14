#!/usr/bin/env python3
"""
Document zip-utils 'unzip' output and behavior across the supported option surface.

Covers Linux-only specification including:
- Operating modes (extract, list, pipe, test, comment)
- Modifiers (overwrite, never-overwrite, junk-paths, quiet)
- Selection (include lists, exclude lists, wildcards)
- Stdin streaming
- Path handling (extraction root, directory creation)
- Negative tests for unsupported features
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
import zipfile
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
    expect_rc: int | None = None


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
    # The zip file to run unzip against (if None, implies implicit or stdin test)
    archive_source: Path | None
    commands: list[CommandSpec]
    notes: list[str]
    # If true, capture the file listing of the workdir after execution
    capture_fs_state: bool = True
    binary_output: bool = False


@dataclass
class ScenarioResult:
    name: str
    description: str
    workdir: str
    fs_state: list[str]  # List of files present in workdir after run
    commands: list[CommandCapture]
    notes: list[str]

    def to_dict(self) -> dict[str, object]:
        return {
            "name": self.name,
            "description": self.description,
            "workdir": self.workdir,
            "fs_state": self.fs_state,
            "commands": [c.to_dict() for c in self.commands],
            "notes": self.notes,
        }


def run(argv: list[str], cwd: Path, stdin: bytes | None, binary_output: bool = False):
    start = datetime.now()
    proc = subprocess.run(
        argv,
        cwd=cwd,
        input=stdin,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    end = datetime.now()

    if binary_output:
        stdout_str = f"<binary output {len(proc.stdout)} bytes>"
    else:
        stdout_str = proc.stdout.decode(errors="replace")

    return (
        proc.returncode,
        stdout_str,
        proc.stderr.decode(errors="replace"),
        int((end - start).total_seconds() * 1000),
    )


def capture_command(spec: CommandSpec, cwd: Path, binary_output: bool = False) -> CommandCapture:
    if spec.before:
        spec.before(cwd)
    rc, out, err, dur = run(spec.argv, cwd, spec.stdin, binary_output)
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


def list_recursive(root: Path) -> list[str]:
    """List all files in root relative to root, sorted."""
    paths = []
    for p in root.rglob("*"):
        if p.is_file() or p.is_symlink():
            paths.append(str(p.relative_to(root)))
    return sorted(paths)


def create_standard_zip(path: Path) -> None:
    """Creates a standard zip file with known content for testing."""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        # Standard file
        zf.writestr("a.txt", "content A\n")
        # Nested file
        zf.writestr("dir/b.txt", "content B\n")
        # Another nested
        zf.writestr("dir/sub/c.dat", "binary content C")
        # File for exclusion testing
        zf.writestr("skip_me.log", "should be skipped")
        # Comment
        zf.comment = b"This is the archive comment"

        # We manually add a symlink entry if running on Unix
        # Python's zipfile doesn't make it easy to write symlink bits explicitly
        # without external calls, so we'll simulate regular files mostly,
        # but try to add a mock symlink if possible or rely on simple extraction.
        # For this spec, we primarily test that unzip *extracts* them,
        # so we'll treat them as files for generation simplicity unless strictly needed.


def build_workdir(root: Path, name: str) -> tuple[Path, Path]:
    workdir = root / name
    workdir.mkdir()
    archive = workdir / "test.zip"
    create_standard_zip(archive)
    return workdir, archive


def build_scenarios(unzip_cmd: str, root: Path) -> list[Scenario]:
    scenarios: list[Scenario] = []
    counter = count(1)

    def new_env(label: str) -> tuple[str, Path, Path, str]:
        name = f"{next(counter):02d}-{label}"
        workdir, archive = build_workdir(root, name)
        return name, workdir, archive, "test.zip"

    # =========================================================================
    # 1. Version & Invocation
    # =========================================================================

    name, workdir, _, _ = new_env("version-check")
    scenarios.append(Scenario(
        name=name,
        description="Check `unzip -v` prints version info and exits 0.",
        workdir=workdir,
        archive_source=None,
        commands=[CommandSpec("version", [unzip_cmd, "-v"], expect_rc=0)],
        notes=["Must exit 0 when no archive is provided."],
        capture_fs_state=False
    ))

    # =========================================================================
    # 2. Operating Modes
    # =========================================================================

    name, workdir, _, aname = new_env("mode-list")
    scenarios.append(Scenario(
        name=name,
        description="List mode (-l).",
        workdir=workdir,
        archive_source=workdir / aname,
        commands=[CommandSpec("list", [unzip_cmd, "-l", aname])],
        notes=["Should output file list (a.txt, dir/b.txt, etc.) without extracting."],
        capture_fs_state=True # Should be empty (except zip)
    ))

    name, workdir, _, aname = new_env("mode-test")
    scenarios.append(Scenario(
        name=name,
        description="Test mode (-t).",
        workdir=workdir,
        archive_source=workdir / aname,
        commands=[CommandSpec("test", [unzip_cmd, "-t", aname])],
        notes=["Verifies CRC checksums. No files created."],
    ))

    name, workdir, _, aname = new_env("mode-comment")
    scenarios.append(Scenario(
        name=name,
        description="Display comment (-z).",
        workdir=workdir,
        archive_source=workdir / aname,
        commands=[CommandSpec("comment", [unzip_cmd, "-z", aname])],
        notes=["Output should contain 'This is the archive comment'."],
        capture_fs_state=False
    ))

    name, workdir, _, aname = new_env("mode-pipe")
    scenarios.append(Scenario(
        name=name,
        description="Pipe extract (-p).",
        workdir=workdir,
        archive_source=workdir / aname,
        commands=[CommandSpec("pipe", [unzip_cmd, "-p", aname, "a.txt"])],
        notes=["Stdout should be 'content A'. No files written."],
        binary_output=True
    ))

    # =========================================================================
    # 3. Extraction & Selection
    # =========================================================================

    name, workdir, _, aname = new_env("extract-default")
    scenarios.append(Scenario(
        name=name,
        description="Default extraction (all files).",
        workdir=workdir,
        archive_source=workdir / aname,
        commands=[CommandSpec("extract", [unzip_cmd, aname])],
        notes=["Should extract a.txt, dir/b.txt, etc."]
    ))

    name, workdir, _, aname = new_env("extract-select")
    scenarios.append(Scenario(
        name=name,
        description="Extract specific files (list argument).",
        workdir=workdir,
        archive_source=workdir / aname,
        commands=[CommandSpec("select", [unzip_cmd, aname, "a.txt"])],
        notes=["Should only extract a.txt."]
    ))

    name, workdir, _, aname = new_env("extract-exclude")
    scenarios.append(Scenario(
        name=name,
        description="Extract with exclude (-x).",
        workdir=workdir,
        archive_source=workdir / aname,
        commands=[CommandSpec("exclude", [unzip_cmd, aname, "-x", "dir/*"])],
        notes=["Should extract a.txt and skip_me.log, but skip dir/ contents."]
    ))

    name, workdir, _, aname = new_env("wildcards")
    scenarios.append(Scenario(
        name=name,
        description="Wildcard matching (* and ?).",
        workdir=workdir,
        archive_source=workdir / aname,
        commands=[CommandSpec("wildcard", [unzip_cmd, aname, "dir/?.*"])],
        notes=["Should match dir/b.txt (assuming ? matches 'b')."]
    ))

    name, workdir, _, aname = new_env("brackets")
    scenarios.append(Scenario(
        name=name,
        description="Bracket pattern matching ([list]).",
        workdir=workdir,
        archive_source=workdir / aname,
        commands=[CommandSpec("brackets", [unzip_cmd, aname, "dir/[b-z].txt"])],
        notes=["Should match dir/b.txt."]
    ))

    # =========================================================================
    # 4. Path Handling & Modifiers
    # =========================================================================

    name, workdir, _, aname = new_env("exdir-flag")
    scenarios.append(Scenario(
        name=name,
        description="Extract to directory (-d).",
        workdir=workdir,
        archive_source=workdir / aname,
        commands=[CommandSpec("exdir", [unzip_cmd, aname, "-d", "outdir"])],
        notes=["Files should be inside outdir/."]
    ))

    name, workdir, _, aname = new_env("exdir-sticky")
    scenarios.append(Scenario(
        name=name,
        description="Extract to directory sticky syntax (-dDIR).",
        workdir=workdir,
        archive_source=workdir / aname,
        commands=[CommandSpec("exdir-stick", [unzip_cmd, aname, "-doutdir"])],
        notes=["Files should be inside outdir/."]
    ))

    name, workdir, _, aname = new_env("junk-paths")
    scenarios.append(Scenario(
        name=name,
        description="Junk paths (-j).",
        workdir=workdir,
        archive_source=workdir / aname,
        commands=[CommandSpec("junk", [unzip_cmd, "-j", aname])],
        notes=["b.txt and c.dat should be in root, dir/ removed."]
    ))

    name, workdir, _, aname = new_env("overwrite-never")
    scenarios.append(Scenario(
        name=name,
        description="Never overwrite (-n).",
        workdir=workdir,
        archive_source=workdir / aname,
        commands=[
            CommandSpec(
                "no-overwrite",
                [unzip_cmd, "-n", aname],
                before=lambda wd: (wd / "a.txt").write_text("ORIGINAL")
            )
        ],
        notes=["a.txt should still contain 'ORIGINAL'. other files extracted."]
    ))

    name, workdir, _, aname = new_env("overwrite-always")
    scenarios.append(Scenario(
        name=name,
        description="Always overwrite (-o).",
        workdir=workdir,
        archive_source=workdir / aname,
        commands=[
            CommandSpec(
                "yes-overwrite",
                [unzip_cmd, "-o", aname],
                before=lambda wd: (wd / "a.txt").write_text("ORIGINAL")
            )
        ],
        notes=["a.txt should contain 'content A'."]
    ))

    # =========================================================================
    # 5. Stdin / Streaming
    # =========================================================================

    name, workdir, archive, _ = new_env("stdin-stream")
    # Read the bytes of the archive to pass to stdin
    with archive.open("rb") as f:
        zip_bytes = f.read()

    scenarios.append(Scenario(
        name=name,
        description="Read archive from stdin (-).",
        workdir=workdir,
        archive_source=None, # Source handled manually via stdin arg
        commands=[CommandSpec(
            "stream-stdin",
            [unzip_cmd, "-"],
            stdin=zip_bytes
        )],
        notes=["Files should be extracted normally."]
    ))

    # =========================================================================
    # 6. Negative Tests (Unsupported Features)
    # =========================================================================

    unsupported_flags = [
        "-C", # Case insensitive
        "-L", # Lowercase
        "-X", # Restore UID/GID
    ]

    for flag in unsupported_flags:
        clean_label = flag.replace("-", "")
        name, workdir, _, aname = new_env(f"fail-{clean_label}")
        scenarios.append(Scenario(
            name=name,
            description=f"Reject unsupported flag {flag}",
            workdir=workdir,
            archive_source=workdir / aname,
            commands=[CommandSpec(
                "expect-fail",
                [unzip_cmd, flag, aname],
                expect_rc=None # Expect non-zero
            )],
            notes=["Should exit with error."],
            capture_fs_state=False
        ))

    return scenarios


def execute_scenario(scenario: Scenario) -> ScenarioResult:
    captures: list[CommandCapture] = []

    # If using stdin bytes from a pre-loaded variable, keep it, otherwise read from archive_source if specific test requires
    # For this harness, we kept it simple: CommandSpec takes bytes.

    for spec in scenario.commands:
        captures.append(capture_command(spec, scenario.workdir, scenario.binary_output))

    fs_state: list[str] = []
    if scenario.capture_fs_state:
        fs_state = list_recursive(scenario.workdir)

    return ScenarioResult(
        name=scenario.name,
        description=scenario.description,
        workdir=str(scenario.workdir),
        fs_state=fs_state,
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
        f.write("# unzip output behavior summary\n\n")
        f.write(f"- command: {metadata.get('unzip_cmd')}\n")
        f.write(f"- version: {metadata.get('unzip_version')}\n")
        f.write(f"- scenarios: {len(results)}\n")
        f.write(f"- timestamp: {metadata.get('generated_at')}\n")

        f.write("\n### Return Codes\n")
        for rc in sorted(by_rc):
            f.write(f"- rc={rc}: {by_rc[rc]} scenario(s)\n")

        f.write("\n## Scenarios\n")
        f.write("| Name | RC | Files | Description |\n")
        f.write("|---|---|---|---|\n")

        for res in results:
            main_rc = res.commands[-1].returncode if res.commands else -1
            file_count = len(res.fs_state)
            desc = res.description.replace("|", "\\|")
            f.write(f"| {res.name} | {main_rc} | {file_count} | {desc} |\n")


def get_unzip_version(cmd: str) -> str:
    proc = subprocess.run(
        [cmd, "-v"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    # Output usually goes to stdout for -v, but if no zipfile is specified some versions vary.
    # The spec says "With only -v... print version info and exit success".
    output = proc.stdout if proc.stdout.strip() else proc.stderr
    if output:
        lines = output.splitlines()
        for line in lines:
            if "UnZip" in line or "Info-ZIP" in line:
                return line.strip()
        return lines[0].strip() if lines else "unknown"
    return f"rc={proc.returncode}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--unzip", default="unzip", help="Path to the unzip binary to document.")
    ap.add_argument("--outdir", default="unzip-doc", help="Output directory for captures.")
    ap.add_argument(
        "--max-runs",
        type=int,
        default=0,
        help="Limit number of scenarios to run (0 = all).",
    )
    args = ap.parse_args()

    if not shutil.which(args.unzip):
        print(f"unzip binary not found: {args.unzip}", file=sys.stderr)
        return 1

    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    unzip_version = get_unzip_version(args.unzip)
    print(f"Documenting: {args.unzip} ({unzip_version})")

    results: list[ScenarioResult] = []

    # Use a temporary directory for all operations
    with tempfile.TemporaryDirectory(prefix="unzip-doc-") as td:
        tmp_root = Path(td)

        scenarios = build_scenarios(args.unzip, tmp_root)
        limit = args.max_runs if args.max_runs > 0 else len(scenarios)

        print(f"Running {limit} scenarios...")
        for i, scenario in enumerate(scenarios[:limit]):
            results.append(execute_scenario(scenario))
        print(f"\nCompleted {len(results)} scenarios.")

    metadata = {
        "unzip_cmd": args.unzip,
        "unzip_version": unzip_version,
        "generated_at": datetime.now().isoformat(),
    }
    write_outputs(outdir, results, metadata)
    print(f"Results written to {outdir}/")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
