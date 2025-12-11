#!/usr/bin/env python3

import os
import subprocess
import tempfile
from pathlib import Path
import zipfile


def run(cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def main():
    zip_bin = os.environ.get("ZIP_BIN", "zip")
    zipinfo_bin = os.environ.get("ZIPINFO_BIN")
    if zipinfo_bin is None:
        zipinfo_bin = str(Path(__file__).resolve().parents[1] / "build" / "unzip")

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "dir").mkdir()
        (tmp_path / "dir" / "a.txt").write_text("alpha\n")
        (tmp_path / "dir" / "b.txt").write_text("bravo\n")
        (tmp_path / "dir" / "blob.bin").write_bytes(b"\x00\x01\x02\x03")
        archive = tmp_path / "sample.zip"

        create = run([zip_bin, "-r", str(archive), "dir"], cwd=tmp_path)
        if create.returncode != 0:
            raise SystemExit(f"zip creation failed: {create.stderr}")
        comment_res = subprocess.run(
            [zip_bin, "-z", str(archive)],
            cwd=tmp_path,
            input="zipinfo archive comment\n",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if comment_res.returncode != 0:
            raise SystemExit(f"zip comment failed: {comment_res.stderr}")

        # Default zipinfo listing should include header, entries, and totals.
        res = run([zipinfo_bin, "-Z", str(archive)])
        if res.returncode != 0:
            raise SystemExit(f"zipinfo default failed: {res.stderr or res.stdout}")
        lines = [ln for ln in res.stdout.strip().splitlines() if ln.strip()]
        if not lines or not lines[0].startswith("Archive:"):
            raise SystemExit("zipinfo default listing missing header")
        if not any("dir/a.txt" in ln for ln in lines):
            raise SystemExit("zipinfo default listing missing entries")
        if "bytes compressed" not in lines[-1]:
            raise SystemExit("zipinfo default listing missing totals footer")
        entry_lines = [ln for ln in lines if ln.strip().startswith("-")]
        flags = {}
        for ln in entry_lines:
            parts = ln.split()
            if len(parts) >= 5:
                flags[parts[-1]] = parts[4]
        if not flags.get("dir/a.txt", "").startswith("t"):
            raise SystemExit(f"expected text flag for a.txt, saw {flags.get('dir/a.txt')}")
        if not flags.get("dir/blob.bin", "").startswith("b"):
            raise SystemExit(f"expected binary flag for blob.bin, saw {flags.get('dir/blob.bin')}")

        # -1 should emit names only, one per line, with no header/footer.
        names_out = run([zipinfo_bin, "-Z", "-1", str(archive)])
        if names_out.returncode != 0:
            raise SystemExit(f"zipinfo -1 failed: {names_out.stderr or names_out.stdout}")
        names = [ln for ln in names_out.stdout.strip().splitlines() if ln]
        with zipfile.ZipFile(archive) as zf:
            expected = sorted(zf.namelist())
        if sorted(names) != expected:
            raise SystemExit(f"zipinfo -1 mismatch\nexpected: {expected}\nactual:   {names}")

        # -t alone should print only the totals footer.
        totals_out = run([zipinfo_bin, "-Z", "-t", str(archive)])
        if totals_out.returncode != 0:
            raise SystemExit(f"zipinfo -t failed: {totals_out.stderr or totals_out.stdout}")
        totals_lines = [ln for ln in totals_out.stdout.strip().splitlines() if ln.strip()]
        if len(totals_lines) != 1 or "bytes compressed" not in totals_lines[0]:
            raise SystemExit(f"zipinfo -t unexpected output: {totals_out.stdout}")

        # Verbose should show extra fields and archive comment.
        verbose = run([zipinfo_bin, "-Z", "-v", str(archive)])
        if verbose.returncode != 0:
            raise SystemExit(f"zipinfo -v failed: {verbose.stderr or verbose.stdout}")
        if "extra fields" not in verbose.stdout or "tag 0x" not in verbose.stdout:
            raise SystemExit("zipinfo -v missing extra field details")
        if "zipfile comment" not in verbose.stdout:
            raise SystemExit("zipinfo -v missing archive comment header")

        # Pager flag should be a no-op in non-tty contexts.
        pager = run([zipinfo_bin, "-Z", "-M", "-1", str(archive)])
        if pager.returncode != 0:
            raise SystemExit(f"zipinfo -M failed: {pager.stderr or pager.stdout}")


if __name__ == "__main__":
    main()
