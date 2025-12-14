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
        archive = work / 'comment-edit.zip'

        zi = zipfile.ZipInfo('keep.txt')
        zi.comment = b'keep entry comment'
        with zipfile.ZipFile(archive, 'w') as zf:
            zf.comment = b'initial archive comment'
            zf.writestr(zi, 'payload')

        updated_comment = b"updated archive comment\nsecond line"
        res = run([zip_bin, '-z', str(archive)], cwd=work, input_data=updated_comment)
        if res.returncode != 0:
            raise SystemExit(f"zip -z edit failed: {res.stderr.decode() or res.stdout.decode()}")

        with zipfile.ZipFile(archive, 'r') as zf:
            if zf.comment != updated_comment:
                raise SystemExit(f"archive comment not updated: {zf.comment!r}")
            info = zf.getinfo('keep.txt')
            if info.comment != b'keep entry comment':
                raise SystemExit(f"entry comment changed: {info.comment!r}")
            if zf.read('keep.txt') != b'payload':
                raise SystemExit("entry payload altered during comment edit")


if __name__ == '__main__':
    main()
