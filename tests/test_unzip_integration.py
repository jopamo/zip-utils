#!/usr/bin/env python3

import os
import subprocess
import tempfile
from pathlib import Path


def run(cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def main():
    zip_bin = os.environ.get('ZIP_BIN', 'zip')
    unzip_bin = os.environ.get('UNZIP_REWRITE_BIN')
    if unzip_bin is None:
        unzip_bin = str(Path(__file__).resolve().parents[1] / 'build' / 'unzip')

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        (tmp_path / 'src').mkdir()
        (tmp_path / 'src' / 'a.txt').write_text('alpha\n')
        (tmp_path / 'src' / 'nested').mkdir()
        (tmp_path / 'src' / 'nested' / 'b.txt').write_text('beta\n')

        archive = tmp_path / 'sample.zip'
        create = run([zip_bin, '-r', str(archive), 'src'], cwd=tmp_path)
        if create.returncode != 0:
            raise SystemExit(f"zip creation failed: {create.stderr}")

        test_res = run([unzip_bin, '-t', str(archive)])
        if test_res.returncode != 0:
            raise SystemExit(f"unzip -t failed: {test_res.stderr or test_res.stdout}")

        dest = tmp_path / 'out'
        dest.mkdir()
        extract = run([unzip_bin, '-d', str(dest), str(archive)])
        if extract.returncode != 0:
            raise SystemExit(f"unzip extract failed: {extract.stderr or extract.stdout}")

        a_path = dest / 'src' / 'a.txt'
        b_path = dest / 'src' / 'nested' / 'b.txt'
        if not a_path.exists() or not b_path.exists():
            raise SystemExit("extracted files missing")
        if a_path.read_text() != 'alpha\n' or b_path.read_text() != 'beta\n':
            raise SystemExit("extracted content mismatch")


if __name__ == '__main__':
    main()
