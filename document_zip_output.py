#!/usr/bin/env python3
"""
Document Info-ZIP 3.0 output and behavior across the full supported option surface.

Covers Linux-only specification including:
- External modes (update, freshen, filesync)
- Internal modes (delete, copy, out)
- Filters (time, include/exclude, recursion patterns)
- Compression methods and levels
- Stream operations
- Argument parsing edge cases
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
from dataclasses import dataclass
from datetime import datetime, timedelta
from itertools import count
from pathlib import Path
from typing import Callable


@dataclass
class CommandSpec:
    label: str
    argv: list[str]
    stdin: bytes | None = None
    before: Callable[[Path], None] | None = None
    # Expected return code is useful for documentation/metadata
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
    archive: Path | None
    commands: list[CommandSpec]
    notes: list[str]
    capture_entries: bool = True
    # If true, the output is binary (e.g., zip to stdout) and shouldn't be decoded strictly
    binary_output: bool = False


@dataclass
class ScenarioResult:
    name: str
    description: str
    workdir: str
    archive_path: str | None
    archive_exists: bool
    archive_size: int
    archive_sha256: str
    archive_mtime: float
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
            "archive_mtime": self.archive_mtime,
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


def list_entries(archive: Path) -> list[str]:
    if not shutil.which("unzip"):
        return []

    # -Z -1 lists filenames only
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
    (root / "dir/deep").mkdir(parents=True)
    (root / "a.txt").write_text("hello\nworld\n")
    (root / "b.bin").write_bytes(os.urandom(256))
    (root / "crlf.txt").write_bytes(b"one\r\ntwo\r\n")
    (root / "data.dat").write_text("database data")
    (root / "script.log").write_text("log data")
    (root / "dir/c.txt").write_text("inside\n")
    (root / "dir/sub/d.txt").write_text("nested\n")
    (root / "dir/deep/e.txt").write_text("deep nested\n")

    # Special files for checklist tests
    (root / "-dash.txt").write_text("file starting with dash\n")
    (root / "pat_a1.txt").write_text("match")
    (root / "pat_b1.txt").write_text("no match")

    try:
        (root / "link").symlink_to("a.txt")
    except OSError:
        pass


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

    # =========================================================================
    # 1. Version & Invocation
    # =========================================================================

    name, workdir, _, _ = new_workdir("version-check")
    scenarios.append(Scenario(
        name=name,
        description="Check `zip -v` prints info and exits 0.",
        workdir=workdir,
        archive=None,
        commands=[CommandSpec("version", [zip_cmd, "-v"], expect_rc=0)],
        notes=["Must exit 0. Output usually contains version info."],
        capture_entries=False
    ))

    # =========================================================================
    # 2. Invocation & Streaming
    # =========================================================================

    name, workdir, _, _ = new_workdir("bare-invocation")
    scenarios.append(Scenario(
        name=name,
        description="Bare invocation (implicit stdin-to-stdout).",
        workdir=workdir,
        archive=None,
        commands=[CommandSpec("bare", [zip_cmd], stdin=b"content")],
        notes=["No args should act like 'zip - -'. Output binary."],
        binary_output=True,
        capture_entries=False
    ))

    name, workdir, archive, aname = new_workdir("stdin-names")
    scenarios.append(Scenario(
        name=name,
        description="Read file names from stdin (-@).",
        workdir=workdir,
        archive=archive,
        commands=[CommandSpec(
            "stdin-list",
            [zip_cmd, "-@", aname],
            stdin=b"a.txt\nb.bin\n"
        )],
        notes=["Should only archive files listed in stdin."]
    ))

    name, workdir, _, _ = new_workdir("stream-stdin-to-file")
    scenarios.append(Scenario(
        name=name,
        description="Stream stdin content to a file archive (zip - out.zip).",
        workdir=workdir,
        archive=workdir / "streamed.zip",
        commands=[CommandSpec(
            "stream-in",
            [zip_cmd, "streamed.zip", "-"],
            stdin=b"streamed content via stdin"
        )],
        notes=["Input '-' triggers stdin reading. Archive created on disk."]
    ))

    name, workdir, _, _ = new_workdir("stream-stdin-to-stdout")
    scenarios.append(Scenario(
        name=name,
        description="Stream stdin content to stdout (zip - -).",
        workdir=workdir,
        archive=None,
        commands=[CommandSpec(
            "stream-stdout",
            [zip_cmd, "-", "-"],
            stdin=b"content"
        )],
        notes=["Both input and output are streams. Output captured as binary blob."],
        binary_output=True,
        capture_entries=False
    ))

    name, workdir, _, _ = new_workdir("stdin-conflict")
    scenarios.append(Scenario(
        name=name,
        description="Conflicting stdin consumers (usage error).",
        workdir=workdir,
        archive=None,
        commands=[CommandSpec(
            "conflict",
            [zip_cmd, "-@", "-", "-"], # -@ wants stdin, inputs want stdin
            stdin=b"data",
            expect_rc=1  # Assuming generic error code
        )],
        notes=["Should fail because -@ and input '-' both want stdin."],
        capture_entries=False
    ))

    # =========================================================================
    # 3. Parsing Rules
    # =========================================================================

    name, workdir, archive, aname = new_workdir("syntax-variations")
    scenarios.append(Scenario(
        name=name,
        description="Grouped options and sticky values.",
        workdir=workdir,
        archive=archive,
        commands=[CommandSpec(
            "grouped",
            # -rq: recurse + quiet
            # -b.: temp dir is '.'
            # -n.txt: suffixes
            [zip_cmd, "-rq", "-b.", "-n.txt", aname, "."]
        )],
        notes=["Parses -rq, -b with value, -n with value correctly."]
    ))

    name, workdir, archive, aname = new_workdir("arg-separator")
    scenarios.append(Scenario(
        name=name,
        description="Argument separator (--).",
        workdir=workdir,
        archive=archive,
        commands=[CommandSpec(
            "dash-file",
            [zip_cmd, aname, "--", "-dash.txt"]
        )],
        notes=["Without --, -dash.txt would be parsed as a flag."]
    ))

    # =========================================================================
    # 4. Operating Modes (External & Internal)
    # =========================================================================

    name, workdir, archive, aname = new_workdir("basic-modes")
    scenarios.append(Scenario(
        name=name,
        description="Basic Create/Add behavior.",
        workdir=workdir,
        archive=archive,
        commands=[CommandSpec("create", [zip_cmd, aname, "a.txt", "b.bin"])],
        notes=[]
    ))

    name, workdir, archive, aname = new_workdir("update-newer")
    scenarios.append(Scenario(
        name=name,
        description="Update (-u) only replaces if source is newer.",
        workdir=workdir,
        archive=archive,
        commands=[
            CommandSpec("seed", [zip_cmd, aname, "a.txt"]),
            CommandSpec(
                "update",
                [zip_cmd, "-u", aname, "a.txt"],
                before=lambda _: (workdir / "a.txt").write_text("updated content")
            ),
        ],
        notes=["Modifies a.txt to ensure timestamp update triggers replacement."]
    ))

    name, workdir, archive, aname = new_workdir("freshen")
    scenarios.append(Scenario(
        name=name,
        description="Freshen (-f) updates existing, ignores new.",
        workdir=workdir,
        archive=archive,
        commands=[
            CommandSpec("seed", [zip_cmd, aname, "a.txt"]),
            CommandSpec(
                "freshen",
                [zip_cmd, "-f", aname, "a.txt", "b.bin"],
                before=lambda _: (workdir / "a.txt").write_text("freshened")
            ),
        ],
        notes=["b.bin is ignored because it's not in the archive."]
    ))

    name, workdir, archive, aname = new_workdir("filesync")
    scenarios.append(Scenario(
        name=name,
        description="Filesync (-FS) adds new, updates changed, deletes missing.",
        workdir=workdir,
        archive=archive,
        commands=[
            CommandSpec("seed", [zip_cmd, aname, "a.txt", "b.bin"]),
            CommandSpec(
                "filesync",
                [zip_cmd, "-FS", aname, "a.txt", "b.bin", "data.dat"],
                before=lambda _: (workdir / "b.bin").unlink()
            ),
        ],
        notes=["Adds data.dat, Keeps a.txt, Removes b.bin (deleted from disk)."]
    ))

    name, workdir, archive, aname = new_workdir("delete-entry")
    scenarios.append(Scenario(
        name=name,
        description="Delete (-d) removes entries from archive.",
        workdir=workdir,
        archive=archive,
        commands=[
            CommandSpec("seed", [zip_cmd, aname, "a.txt", "dir/c.txt"]),
            CommandSpec("delete", [zip_cmd, "-d", aname, "dir/*"]),
        ],
        notes=["Should remove dir/c.txt but keep a.txt."]
    ))

    name, workdir, archive, aname = new_workdir("delete-filtered")
    scenarios.append(Scenario(
        name=name,
        description="Delete mode restricted by time (-d -tt).",
        workdir=workdir,
        archive=archive,
        commands=[
            CommandSpec("seed", [zip_cmd, aname, "a.txt", "b.bin"]),
            # Delete entries older than 2020.
            CommandSpec(
                "delete-old",
                [zip_cmd, "-d", "-tt", "2020-01-01", aname, "*"],
                before=lambda _: set_mtime(workdir / "a.txt", datetime(2010, 1, 1))
            )
        ],
        notes=["a.txt (2010) should be deleted. b.bin (now) preserved."]
    ))

    name, workdir, archive, aname = new_workdir("copy-out")
    scenarios.append(Scenario(
        name=name,
        description="Copy entries (-U) to new archive (--out).",
        workdir=workdir,
        archive=workdir / "final.zip",
        commands=[
            CommandSpec("seed", [zip_cmd, "source.zip", "a.txt", "b.bin"]),
            CommandSpec("copy", [zip_cmd, "-U", "source.zip", "--out", "final.zip", "a.txt"]),
        ],
        notes=["Selects only a.txt from source.zip to write to final.zip."]
    ))

    # =========================================================================
    # 5. Traversal & Paths
    # =========================================================================

    name, workdir, archive, aname = new_workdir("recurse-standard")
    scenarios.append(Scenario(
        name=name,
        description="Standard recursion (-r).",
        workdir=workdir,
        archive=archive,
        commands=[CommandSpec("recurse", [zip_cmd, "-r", aname, "dir"])],
        notes=[]
    ))

    name, workdir, archive, aname = new_workdir("recurse-patterns")
    scenarios.append(Scenario(
        name=name,
        description="Recursion with patterns (-R).",
        workdir=workdir,
        archive=archive,
        commands=[CommandSpec("recurse-pats", [zip_cmd, "-R", aname, "*.txt"])],
        notes=["Should find a.txt, dir/c.txt, etc., but skip b.bin."]
    ))

    name, workdir, archive, aname = new_workdir("junk-paths")
    scenarios.append(Scenario(
        name=name,
        description="Junk paths (-j).",
        workdir=workdir,
        archive=archive,
        commands=[CommandSpec("junk", [zip_cmd, "-j", aname, "dir/c.txt"])],
        notes=["Stores c.txt at root, ignoring 'dir/'."]
    ))

    name, workdir, archive, aname = new_workdir("no-dir-entries")
    scenarios.append(Scenario(
        name=name,
        description="Suppress directory entries (-D).",
        workdir=workdir,
        archive=archive,
        commands=[CommandSpec("no-dirs", [zip_cmd, "-r", "-D", aname, "dir"])],
        notes=["Files are added, but explicit directory nodes are skipped."]
    ))

    name, workdir, archive, aname = new_workdir("symlinks-store")
    scenarios.append(Scenario(
        name=name,
        description="Store symlinks as links (-y).",
        workdir=workdir,
        archive=archive,
        commands=[CommandSpec("symlinks", [zip_cmd, "-y", aname, "link"])],
        notes=["Should store the link 'link', not the content of 'a.txt'."]
    ))

    # =========================================================================
    # 6. Include / Exclude
    # =========================================================================

    name, workdir, archive, aname = new_workdir("include-filter")
    scenarios.append(Scenario(
        name=name,
        description="Include filter (-i).",
        workdir=workdir,
        archive=archive,
        commands=[CommandSpec("include", [zip_cmd, "-r", aname, ".", "-i", "*.txt"])],
        notes=["Recurses current dir but only includes .txt files."]
    ))

    name, workdir, archive, aname = new_workdir("exclude-filter")
    scenarios.append(Scenario(
        name=name,
        description="Exclude filter (-x).",
        workdir=workdir,
        archive=archive,
        commands=[CommandSpec("exclude", [zip_cmd, "-r", aname, ".", "-x", "*.bin", "*.dat"])],
        notes=["Recurses current dir but excludes .bin and .dat files."]
    ))

    name, workdir, archive, aname = new_workdir("pattern-brackets")
    scenarios.append(Scenario(
        name=name,
        description="Bracket pattern matching [a-z].",
        workdir=workdir,
        archive=archive,
        commands=[CommandSpec("brackets", [zip_cmd, aname, "pat_[a-z]1.txt"])],
        notes=["Should match pat_a1.txt and pat_b1.txt, ignore pat_a2.txt."]
    ))

    # =========================================================================
    # 8. Date Filtering
    # =========================================================================

    name, workdir, archive, aname = new_workdir("time-filter-after")
    scenarios.append(Scenario(
        name=name,
        description="Include after date (-t).",
        workdir=workdir,
        archive=archive,
        commands=[
            CommandSpec(
                "filter-t",
                [zip_cmd, "-t", "2020-01-01", aname, "a.txt", "b.bin"],
                before=lambda _: set_mtime(workdir / "a.txt", datetime(2010, 1, 1))
            )
        ],
        notes=["a.txt (2010) is too old. Only b.bin should remain (default current time)."]
    ))

    name, workdir, archive, aname = new_workdir("time-filter-before")
    scenarios.append(Scenario(
        name=name,
        description="Include before date (-tt).",
        workdir=workdir,
        archive=archive,
        commands=[
            CommandSpec(
                "filter-tt",
                [zip_cmd, "-tt", "2015-01-01", aname, "a.txt", "b.bin"],
                before=lambda _: set_mtime(workdir / "a.txt", datetime(2010, 1, 1))
            )
        ],
        notes=["a.txt (2010) matches. b.bin (current) is too new."]
    ))

    # =========================================================================
    # 9. Compression
    # =========================================================================

    name, workdir, archive, aname = new_workdir("compression-store")
    scenarios.append(Scenario(
        name=name,
        description="Store only (-0).",
        workdir=workdir,
        archive=archive,
        commands=[CommandSpec("store", [zip_cmd, "-0", aname, "a.txt"])],
        notes=["Forces 0% compression."]
    ))

    name, workdir, archive, aname = new_workdir("compression-max")
    scenarios.append(Scenario(
        name=name,
        description="Max compression (-9).",
        workdir=workdir,
        archive=archive,
        commands=[CommandSpec("max", [zip_cmd, "-9", aname, "b.bin"])],
        notes=["Forces Deflate level 9."]
    ))

    name, workdir, archive, aname = new_workdir("compression-method")
    scenarios.append(Scenario(
        name=name,
        description="Force compression method (-Z store).",
        workdir=workdir,
        archive=archive,
        commands=[CommandSpec("force-store", [zip_cmd, "-Z", "store", aname, "a.txt"])],
        notes=["Explicitly sets method to Store."]
    ))

    name, workdir, archive, aname = new_workdir("suffixes")
    scenarios.append(Scenario(
        name=name,
        description="No compression for suffixes (-n).",
        workdir=workdir,
        archive=archive,
        commands=[CommandSpec("suffixes", [zip_cmd, "-n", ".txt:.dat", aname, "a.txt", "data.dat", "b.bin"])],
        notes=[".txt and .dat should be Stored. .bin should be Deflated."]
    ))

    # =========================================================================
    # 10. Text Translation
    # =========================================================================

    name, workdir, archive, aname = new_workdir("text-lf-crlf")
    scenarios.append(Scenario(
        name=name,
        description="Convert LF to CRLF (-l).",
        workdir=workdir,
        archive=archive,
        commands=[CommandSpec("eol-crlf", [zip_cmd, "-l", aname, "a.txt"])],
        notes=["a.txt contents should be converted. Size/CRC will change."]
    ))

    name, workdir, archive, aname = new_workdir("text-crlf-lf")
    scenarios.append(Scenario(
        name=name,
        description="Convert CRLF to LF (-ll).",
        workdir=workdir,
        archive=archive,
        commands=[CommandSpec("eol-lf", [zip_cmd, "-ll", aname, "crlf.txt"])],
        notes=["crlf.txt (Windows) should become LF (Unix)."]
    ))

    # =========================================================================
    # 11, 12, 14, 16. Metadata & Operational
    # =========================================================================

    name, workdir, archive, aname = new_workdir("quiet-mode")
    scenarios.append(Scenario(
        name=name,
        description="Quiet operation (-q).",
        workdir=workdir,
        archive=archive,
        commands=[CommandSpec("quiet", [zip_cmd, "-q", aname, "a.txt"])],
        notes=["Stdout should be empty."]
    ))

    name, workdir, archive, aname = new_workdir("temp-path")
    scenarios.append(Scenario(
        name=name,
        description="Use temporary directory (-b).",
        workdir=workdir,
        archive=archive,
        commands=[CommandSpec("temp", [zip_cmd, "-b", str(workdir), aname, "a.txt"])],
        notes=["Uses workdir for temp files. Hard to observe externally, but checks for crash."]
    ))

    name, workdir, archive, aname = new_workdir("test-integrity")
    scenarios.append(Scenario(
        name=name,
        description="Basic integrity check (-T).",
        workdir=workdir,
        archive=archive,
        commands=[
            CommandSpec("seed", [zip_cmd, aname, "a.txt"]),
            CommandSpec("check", [zip_cmd, "-T", aname])
        ],
        notes=["Should verify the archive without extracting."]
    ))

    name, workdir, archive, aname = new_workdir("test-custom")
    scenarios.append(Scenario(
        name=name,
        description="Test with custom command (-TT).",
        workdir=workdir,
        archive=archive,
        commands=[
            CommandSpec("seed", [zip_cmd, aname, "a.txt"]),
            CommandSpec("test-cmd", [zip_cmd, "-T", "-TT", "ls -l {}", aname]),
        ],
        notes=["Should execute 'ls -l <tmpzip>'."],
        binary_output=True,
    ))

    name, workdir, archive, aname = new_workdir("set-mtime")
    # We set a file 1 hour in the future. -o should make the zip file match that time.
    future_time = datetime.now() + timedelta(hours=1)
    scenarios.append(Scenario(
        name=name,
        description="Set zipfile time to newest entry (-o).",
        workdir=workdir,
        archive=archive,
        commands=[
            CommandSpec(
                "set-time",
                [zip_cmd, "-o", aname, "a.txt"],
                before=lambda _: set_mtime(workdir / "a.txt", future_time)
            )
        ],
        notes=["Archive mtime should match a.txt mtime."]
    ))

    name, workdir, archive, aname = new_workdir("strip-extra")
    scenarios.append(Scenario(
        name=name,
        description="Strip extra attributes (-X).",
        workdir=workdir,
        archive=archive,
        commands=[CommandSpec("strip", [zip_cmd, "-X", aname, "a.txt"])],
        notes=["Resulting archive entries should lack extra fields (UID/GID etc)."]
    ))

    name, workdir, archive, aname = new_workdir("move-files")
    scenarios.append(Scenario(
        name=name,
        description="Move files into archive (-m).",
        workdir=workdir,
        archive=archive,
        commands=[CommandSpec("move", [zip_cmd, "-m", aname, "a.txt"])],
        notes=["a.txt should be deleted from disk after archiving."]
    ))

    return scenarios


def execute_scenario(scenario: Scenario) -> ScenarioResult:
    captures: list[CommandCapture] = []
    for spec in scenario.commands:
        captures.append(capture_command(spec, scenario.workdir, scenario.binary_output))

    archive_exists = scenario.archive is not None and scenario.archive.exists()
    archive_size = 0
    archive_sha = ""
    archive_mtime = 0.0
    entries: list[str] = []

    if archive_exists:
        stat = scenario.archive.stat()
        archive_size = stat.st_size
        archive_mtime = stat.st_mtime
        archive_sha = sha256_file(scenario.archive)
        if scenario.capture_entries:
            entries = list_entries(scenario.archive)

    return ScenarioResult(
        name=scenario.name,
        description=scenario.description,
        workdir=str(scenario.workdir),
        archive_path=str(scenario.archive) if scenario.archive else None,
        archive_exists=archive_exists,
        archive_size=archive_size,
        archive_sha256=archive_sha,
        archive_mtime=archive_mtime,
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
        f.write(f"- timestamp: {metadata.get('generated_at')}\n")
        f.write("\n### Return Codes\n")
        for rc in sorted(by_rc):
            f.write(f"- rc={rc}: {by_rc[rc]} scenario(s)\n")

        f.write("\n## Scenarios\n")
        f.write("| Name | RC | Archive | Entries | Description |\n")
        f.write("|---|---|---|---|---|\n")

        for res in results:
            main_rc = res.commands[-1].returncode if res.commands else -1
            entry_count = len(res.entries)
            archive_state = "Yes" if res.archive_exists else "No"
            # Escape pipes in description for markdown table safety
            desc = res.description.replace("|", "\\|")
            f.write(f"| {res.name} | {main_rc} | {archive_state} | {entry_count} | {desc} |\n")


def get_zip_version(zip_cmd: str) -> str:
    # Try -v first (standard info-zip)
    proc = subprocess.run(
        [zip_cmd, "-v"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    # Some versions output to stdout, some to stderr if no zipfile provided
    output = proc.stdout if proc.stdout.strip() else proc.stderr
    if output:
        lines = output.splitlines()
        for line in lines:
            if "Zip" in line and "Info-ZIP" in line:
                return line.strip()
        return lines[0].strip() if lines else "unknown"
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
    print(f"Documenting: {args.zip} ({zip_version})")

    results: list[ScenarioResult] = []
    with tempfile.TemporaryDirectory(prefix="zip-doc-") as td:
        tmp_root = Path(td)
        fixture = tmp_root / "fixture"
        fixture.mkdir()
        make_fixture(fixture)

        scenarios = build_scenarios(args.zip, fixture, tmp_root)
        limit = args.max_runs if args.max_runs > 0 else len(scenarios)

        print(f"Running {limit} scenarios...")
        for i, scenario in enumerate(scenarios[:limit]):
            results.append(execute_scenario(scenario))
        print(f"\nCompleted {len(results)} scenarios.")

    metadata = {
        "zip_cmd": args.zip,
        "zip_version": zip_version,
        "generated_at": datetime.now().isoformat(),
    }
    write_outputs(outdir, results, metadata)
    print(f"Results written to {outdir}/")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
