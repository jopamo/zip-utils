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
        (tmp_path / 'src' / 'keep.txt').write_text('keep\n')
        (tmp_path / 'src' / 'skip.txt').write_text('skip\n')
        archive = tmp_path / 'sample.zip'

        create = run([zip_bin, '-r', str(archive), 'src'], cwd=tmp_path)
        if create.returncode != 0:
            raise SystemExit(f"zip creation failed: {create.stderr}")

        dest = tmp_path / 'out'
        dest.mkdir()
        # exclude skip.txt
        extract = run([unzip_bin, '-d', str(dest), '-x', 'src/skip.txt', str(archive)])
        if extract.returncode != 0:
            raise SystemExit(f"unzip extract failed: {extract.stderr or extract.stdout}")

        if (dest / 'src' / 'skip.txt').exists():
            raise SystemExit("excluded file was extracted")
        if not (dest / 'src' / 'keep.txt').exists():
            raise SystemExit("included file missing")


if __name__ == '__main__':
    main()
