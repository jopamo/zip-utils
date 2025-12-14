#!/usr/bin/env python3

import os
import subprocess
import tempfile
from pathlib import Path
import zipfile


def run(cmd, cwd=None, input_data: bytes | None = None):
    return subprocess.run(cmd, cwd=cwd, input=input_data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=False)


def main():
    zip_bin = os.environ.get('WRITE_BIN')
    if not zip_bin:
        zip_bin = str(Path(__file__).resolve().parents[1] / 'build' / 'zip')

    with tempfile.TemporaryDirectory() as tmpdir:
        work = Path(tmpdir)
        a = work / 'a.txt'
        b = work / 'b.txt'
        a.write_text('hello move a')
        b.write_text('hello move b')

        archive = work / 'moved.zip'
        res = run([zip_bin, '-m', str(archive), a.name, b.name], cwd=work)
        if res.returncode != 0:
            raise SystemExit(f"zip -m failed: {res.stderr.decode() or res.stdout.decode()}")

        if a.exists() or b.exists():
            raise SystemExit("source files were not removed after -m")

        with zipfile.ZipFile(archive, 'r') as zf:
            names = sorted(zf.namelist())
            if names != ['a.txt', 'b.txt']:
                raise SystemExit(f"archive entries incorrect: {names}")
            if zf.read('a.txt') != b'hello move a' or zf.read('b.txt') != b'hello move b':
                raise SystemExit("archived contents do not match originals")

        # Ensure stdin paths are not removed (should be skipped gracefully)
        archive2 = work / 'stdin.zip'
        res2 = run([zip_bin, '-m', str(archive2), '-'], cwd=work, input_data=b'stdin data')
        if res2.returncode != 0:
            raise SystemExit(f"zip -m with stdin failed: {res2.stderr.decode() or res2.stdout.decode()}")
        with zipfile.ZipFile(archive2, 'r') as zf:
            if zf.read('-') != b'stdin data':
                raise SystemExit("stdin archive content mismatch")


if __name__ == '__main__':
    main()
