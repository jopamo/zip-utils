#!/usr/bin/env python3

import os
import subprocess
import tempfile
from pathlib import Path
import zipfile


def run(cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def main():
    zip_bin = os.environ.get('WRITE_BIN')
    if zip_bin is None:
        zip_bin = str(Path(__file__).resolve().parents[1] / 'build' / 'zip')
    unzip_bin = os.environ.get('UNZIP_BIN', 'unzip')

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        payload_dir = tmp_path / 'payload'
        payload_dir.mkdir()
        (payload_dir / 'a.txt').write_text('hello ziputils\n')
        (payload_dir / 'sub').mkdir()
        (payload_dir / 'sub' / 'b.txt').write_text('subdir content')

        archive = tmp_path / 'out.zip'
        create = run([zip_bin, str(archive), 'payload/a.txt', 'payload/sub/b.txt'], cwd=tmp_path)
        if create.returncode != 0:
            raise SystemExit(f"zip creation failed: {create.stderr}")

        test_res = run([unzip_bin, '-t', str(archive)])
        if test_res.returncode != 0:
            raise SystemExit(f"unzip -t failed: {test_res.stderr or test_res.stdout}")

        with zipfile.ZipFile(archive, 'r') as zf:
            names = sorted(zf.namelist())
            expected = ['payload/a.txt', 'payload/sub/b.txt']
            if names != expected:
                raise SystemExit(f"names mismatch: expected {expected}, got {names}")

            if zf.read('payload/a.txt').decode() != 'hello ziputils\n':
                raise SystemExit("payload/a.txt content mismatch")
            if zf.read('payload/sub/b.txt').decode() != 'subdir content':
                raise SystemExit("payload/sub/b.txt content mismatch")


if __name__ == '__main__':
    main()
