#!/usr/bin/env python3

import os
import subprocess
import tempfile
import zipfile
from pathlib import Path


def run(cmd, cwd=None, input_data: bytes | None = None):
    return subprocess.run(cmd, cwd=cwd, input=input_data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=False)


def main():
    zip_bin = os.environ.get('WRITE_BIN')
    if not zip_bin:
        zip_bin = str(Path(__file__).resolve().parents[1] / 'build' / 'zip')

    with tempfile.TemporaryDirectory() as tmpdir:
        work = Path(tmpdir)
        f1 = work / 'f1.txt'
        f1.write_text('hello comment')

        comment = b"archive comment\nsecond line"
        archive = work / 'commented.zip'
        res = run([zip_bin, '-z', str(archive), f1.name], cwd=work, input_data=comment)
        if res.returncode != 0:
            raise SystemExit(f"zip -z failed: {res.stderr.decode() or res.stdout.decode()}")

        with zipfile.ZipFile(archive, 'r') as zf:
            if zf.comment != comment:
                raise SystemExit(f"archive comment mismatch: {zf.comment!r} != {comment!r}")
            if zf.read('f1.txt') != b'hello comment':
                raise SystemExit("entry content mismatch after comment write")

        # Add a second file without -z and ensure the comment is preserved
        f2 = work / 'f2.txt'
        f2.write_text('second')
        res2 = run([zip_bin, str(archive), f2.name], cwd=work)
        if res2.returncode != 0:
            raise SystemExit(f"zip update failed: {res2.stderr.decode() or res2.stdout.decode()}")

        with zipfile.ZipFile(archive, 'r') as zf:
            if zf.comment != comment:
                raise SystemExit("archive comment was not preserved during rewrite")
            names = sorted(zf.namelist())
            if names != ['f1.txt', 'f2.txt']:
                raise SystemExit(f"unexpected names after update: {names}")


if __name__ == '__main__':
    main()
