#!/usr/bin/env python3

import os
import subprocess
import tempfile
from pathlib import Path
import zipfile


def run(cmd, cwd=None):
    res = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res.returncode != 0:
        raise RuntimeError(f"cmd failed {cmd}: {res.stderr}")
    return res.stdout


def main():
    zip_bin = os.environ.get('ZIP_BIN', 'zip')
    reader_bin = os.environ.get('READER_BIN', None)
    if reader_bin is None:
        reader_bin = str(Path(__file__).resolve().parents[1] / 'build' / 'unzip')
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        (tmp_path / 'a.txt').write_text('hello')
        (tmp_path / 'b/b.txt').parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / 'b/b.txt').write_text('world')
        archive = tmp_path / 'sample.zip'
        run([zip_bin, '-r', str(archive), 'a.txt', 'b'], cwd=tmp_path)

        with zipfile.ZipFile(archive, 'r') as zf:
            expected = sorted(zf.namelist())

        ours = run([reader_bin, '-l', str(archive)]).splitlines()
        if expected != ours:
            raise SystemExit(f"listing mismatch\nexpected: {expected}\nactual:   {ours}")

        quiet_out = run([reader_bin, '-ql', str(archive)]).strip()
        if quiet_out:
            raise SystemExit(f"quiet listing not quiet: {quiet_out}")

        verbose_out = run([reader_bin, '-vl', str(archive)]).splitlines()
        # Accept either "Total entries:" (generic) or "N files, ... compressed" (zipinfo)
        if not verbose_out:
             raise SystemExit("verbose listing missing output")
             
        last_line = verbose_out[-1]
        has_summary = last_line.startswith('Total entries:') or ('files' in last_line and 'compressed' in last_line)
        
        if not has_summary:
            raise SystemExit(f"verbose listing missing summary: {last_line}")


if __name__ == '__main__':
    main()
