#!/usr/bin/env python3

import os
import subprocess
import tempfile
from pathlib import Path


def run(cmd, cwd=None, text=True):
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=text)


def main():
    zip_bin = os.environ.get('WRITE_BIN') or str(Path(__file__).resolve().parents[1] / 'build' / 'zip')
    unzip_bin = os.environ.get('UNZIP_BIN') or str(Path(__file__).resolve().parents[1] / 'build' / 'unzip')

    with tempfile.TemporaryDirectory() as tmpdir:
        work = Path(tmpdir)
        archive = work / 'split.zip'

        a = work / 'a.txt'
        b = work / 'b.txt'
        a.write_bytes(os.urandom(4096))
        b.write_bytes(os.urandom(4096))

        res_zip = run([zip_bin, '-0', '-s', '1k', str(archive), a.name, b.name], cwd=work)
        if res_zip.returncode != 0:
            raise SystemExit(f"split zip create failed: {res_zip.stderr or res_zip.stdout}")

        first_part = archive.with_suffix('.z01')
        if not first_part.exists():
            raise SystemExit("split archive first part (.z01) missing after write")

        list_res = run([unzip_bin, '-l', str(archive)], cwd=work)
        if list_res.returncode != 0:
            raise SystemExit(f"split archive list failed: {list_res.stderr or list_res.stdout}")
        if 'a.txt' not in list_res.stdout or 'b.txt' not in list_res.stdout:
            raise SystemExit(f"list output missing entries: {list_res.stdout}")

        out_dir = work / 'out'
        out_dir.mkdir()
        extract_res = run([unzip_bin, str(archive), '-d', str(out_dir)], cwd=work)
        if extract_res.returncode != 0:
            raise SystemExit(f"split archive extract failed: {extract_res.stderr or extract_res.stdout}")

        if (out_dir / 'a.txt').read_bytes() != a.read_bytes():
            raise SystemExit("extracted a.txt content mismatch")
        if (out_dir / 'b.txt').read_bytes() != b.read_bytes():
            raise SystemExit("extracted b.txt content mismatch")


if __name__ == '__main__':
    main()
