#!/usr/bin/env python3
"""
Document zipinfo output/behavior across key listing formats and flags.

Covers Linux-only specification including:
- Listing formats (-1, -2, -s, -m, -v)
- Header/footer toggles (-h, pattern-driven defaults)
- Timestamp formatting (-T)
- Comment visibility (-z)
- Stub toggles (-M pager)
"""

from __future__ import annotations

import argparse
import json
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
    archive_source: Path | None
    commands: list[CommandSpec]
    notes: list[str]
    capture_fs_state: bool = False
    binary_output: bool = False


@dataclass
class ScenarioResult:
    name: str
    description: str
    workdir: str
    fs_state: list[str]
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

    stdout_str = f"<binary output {len(proc.stdout)} bytes>" if binary_output else proc.stdout.decode(errors="replace")

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
    paths = []
    for p in root.rglob("*"):
        if p.is_file() or p.is_symlink():
            paths.append(str(p.relative_to(root)))
    return sorted(paths)


def create_standard_zip(path: Path) -> None:
    """Creates a standard zip file with known content and an archive comment."""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("a.txt", "content A\n")
        zf.writestr("dir/b.txt", "content B\n")
        zf.writestr("dir/sub/c.dat", "binary content C")
        zf.writestr("skip_me.log", "should be skipped")
        zf.comment = b"This is the archive comment"


def build_workdir(root: Path, name: str) -> tuple[Path, Path]:
    workdir = root / name
    workdir.mkdir()
    archive = workdir / "test.zip"
    create_standard_zip(archive)
    return workdir, archive


def build_scenarios(zipinfo_cmd: list[str], root: Path) -> list[Scenario]:
    scenarios: list[Scenario] = []
    counter = count(1)

    def new_env(label: str) -> tuple[str, Path, Path, str]:
        name = f"{next(counter):02d}-{label}"
        workdir, archive = build_workdir(root, name)
        return name, workdir, archive, archive.name

    def cmd(*args: str) -> list[str]:
        return zipinfo_cmd + list(args)

    # Version check (no archive)
    name, workdir, archive, archive_rel = new_env("version-check")
    scenarios.append(Scenario(
        name=name,
        description="zipinfo -v prints version info with no archive.",
        workdir=workdir,
        archive_source=None,
        commands=[CommandSpec("version", cmd("-v"), expect_rc=0)],
        notes=["Expected to exit 0 without needing an archive."],
        capture_fs_state=False,
    ))

    # Default short listing
    name, workdir, archive, archive_rel = new_env("short-list")
    scenarios.append(Scenario(
        name=name,
        description="Short listing default with header/footer.",
        workdir=workdir,
        archive_source=archive,
        commands=[CommandSpec("short", cmd(archive_rel), expect_rc=0)],
        notes=["Baseline listing shape."],
        capture_fs_state=False,
    ))

    # Names only (-1)
    name, workdir, archive, archive_rel = new_env("names-only-quiet")
    scenarios.append(Scenario(
        name=name,
        description="Names-only quiet listing (-1) suppresses header/footer.",
        workdir=workdir,
        archive_source=archive,
        commands=[CommandSpec("names-quiet", cmd("-1", archive_rel), expect_rc=0)],
        notes=["Useful for diffing entry names only."],
        capture_fs_state=False,
    ))

    # Names only allowing headers (-2)
    name, workdir, archive, archive_rel = new_env("names-with-header")
    scenarios.append(Scenario(
        name=name,
        description="Names-only listing (-2) may emit headers/footers.",
        workdir=workdir,
        archive_source=archive,
        commands=[CommandSpec("names-header", cmd("-2", archive_rel), expect_rc=0)],
        notes=["Captures header/footer presence in names-only mode."],
        capture_fs_state=False,
    ))

    # Medium listing (-m)
    name, workdir, archive, archive_rel = new_env("medium-list")
    scenarios.append(Scenario(
        name=name,
        description="Medium listing (-m) shows sizes and timestamps.",
        workdir=workdir,
        archive_source=archive,
        commands=[CommandSpec("medium", cmd("-m", archive_rel), expect_rc=0)],
        notes=[],
        capture_fs_state=False,
    ))

    # Verbose listing (-v with archive)
    name, workdir, archive, archive_rel = new_env("verbose-list")
    scenarios.append(Scenario(
        name=name,
        description="Verbose listing (-v with archive) includes comments and extra fields.",
        workdir=workdir,
        archive_source=archive,
        commands=[CommandSpec("verbose", cmd("-v", archive_rel), expect_rc=0)],
        notes=["Same flag as version mode but with archive present."],
        capture_fs_state=False,
    ))

    # Decimal timestamps (-T)
    name, workdir, archive, archive_rel = new_env("decimal-time")
    scenarios.append(Scenario(
        name=name,
        description="Decimal timestamps (-T) adjust time formatting.",
        workdir=workdir,
        archive_source=archive,
        commands=[CommandSpec("decimal-time", cmd("-T", archive_rel), expect_rc=0)],
        notes=[],
        capture_fs_state=False,
    ))

    # Show comments (-z)
    name, workdir, archive, archive_rel = new_env("show-comments")
    scenarios.append(Scenario(
        name=name,
        description="Show archive and entry comments (-z).",
        workdir=workdir,
        archive_source=archive,
        commands=[CommandSpec("comments", cmd("-z", archive_rel), expect_rc=0)],
        notes=["Archive comment present; entry comments may be absent."],
        capture_fs_state=False,
    ))

    # Pattern listing suppresses header/footer unless forced
    name, workdir, archive, archive_rel = new_env("pattern-header-defaults")
    scenarios.append(Scenario(
        name=name,
        description="Pattern listing drops header/footer by default.",
        workdir=workdir,
        archive_source=archive,
        commands=[CommandSpec("pattern", cmd(archive_rel, "dir/*"), expect_rc=0)],
        notes=["Info-ZIP suppresses headers when patterns narrow the listing."],
        capture_fs_state=False,
    ))

    # Force header/footer with -h when using patterns
    name, workdir, archive, archive_rel = new_env("pattern-with-header")
    scenarios.append(Scenario(
        name=name,
        description="Pattern listing with -h restores header/footer.",
        workdir=workdir,
        archive_source=archive,
        commands=[CommandSpec("pattern-header", cmd("-h", archive_rel, "dir/*"), expect_rc=0)],
        notes=["Demonstrates header toggle behavior with explicit request."],
        capture_fs_state=False,
    ))

    # Pager flag (-M) is a stub/no-op
    name, workdir, archive, archive_rel = new_env("pager-stub")
    scenarios.append(Scenario(
        name=name,
        description="Pager toggle (-M) is a stub in zip-utils.",
        workdir=workdir,
        archive_source=archive,
        commands=[CommandSpec("pager", cmd("-M", archive_rel), expect_rc=0)],
        notes=["Output should note pager handling if implemented; otherwise no-op."],
        capture_fs_state=False,
    ))

    return scenarios


def execute_scenario(scenario: Scenario) -> ScenarioResult:
    captures: list[CommandCapture] = []
    for spec in scenario.commands:
        captures.append(capture_command(spec, scenario.workdir, scenario.binary_output))

    fs_state: list[str] = list_recursive(scenario.workdir) if scenario.capture_fs_state else []

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
        f.write("# zipinfo output behavior summary\n\n")
        f.write(f"- command: {metadata.get('zipinfo_cmd')}\n")
        f.write(f"- version: {metadata.get('zipinfo_version')}\n")
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


def get_zipinfo_version(cmd: list[str]) -> str:
    proc = subprocess.run(
        cmd + ["-v"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    output = proc.stdout if proc.stdout.strip() else proc.stderr
    if output:
        lines = output.splitlines()
        for line in lines:
            if "ZipInfo" in line or "zipinfo" in line or "Info-ZIP" in line:
                return line.strip()
        return lines[0].strip() if lines else "unknown"
    return f"rc={proc.returncode}"


def main() -> int:
    ap = argparse.ArgumentParser(allow_abbrev=False)
    ap.add_argument("--zipinfo", default="zipinfo", help="Path to the zipinfo (or unzip) binary to document.")
    ap.add_argument("--zipinfo-arg", action="append", default=[], help="Extra arg for zipinfo invocation (repeatable, e.g. --zipinfo-arg -Z).")
    ap.add_argument("--outdir", default="zipinfo-doc", help="Output directory for captures.")
    ap.add_argument("--max-runs", type=int, default=0, help="Limit number of scenarios to run (0 = all).")
    args = ap.parse_args()

    if not shutil.which(args.zipinfo):
        print(f"zipinfo binary not found: {args.zipinfo}", file=sys.stderr)
        return 1

    zipinfo_cmd = [args.zipinfo] + args.zipinfo_arg

    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    zipinfo_version = get_zipinfo_version(zipinfo_cmd)
    print(f"Documenting: {' '.join(zipinfo_cmd)} ({zipinfo_version})")

    results: list[ScenarioResult] = []

    with tempfile.TemporaryDirectory(prefix="zipinfo-doc-") as td:
        tmp_root = Path(td)

        scenarios = build_scenarios(zipinfo_cmd, tmp_root)
        limit = args.max_runs if args.max_runs > 0 else len(scenarios)

        print(f"Running {limit} scenarios...")
        for scenario in scenarios[:limit]:
            results.append(execute_scenario(scenario))
        print(f"\nCompleted {len(results)} scenarios.")

    metadata = {
        "zipinfo_cmd": " ".join(zipinfo_cmd),
        "zipinfo_version": zipinfo_version,
        "generated_at": datetime.now().isoformat(),
    }
    write_outputs(outdir, results, metadata)
    print(f"Results written to {outdir}/")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
