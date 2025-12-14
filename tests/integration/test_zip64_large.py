#!/usr/bin/env python3

import os
import subprocess
import tempfile
from pathlib import Path


def run(cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def requires_env():
    return os.environ.get('ZU_RUN_LARGE_TESTS') == '1'


def expect_signatures(path: Path):
    blob = path.read_bytes()
    has_eocd64 = b'\x50\x4b\x06\x06' in blob
    has_locator = b'\x50\x4b\x06\x07' in blob
    if not (has_eocd64 and has_locator):
        raise SystemExit("zip64 signatures missing")


def main():
    if not requires_env():
        print("skipping large Zip64 test (set ZU_RUN_LARGE_TESTS=1 to enable)")
        return

    zip_bin = os.environ.get('WRITE_BIN')
    if zip_bin is None:
        zip_bin = str(Path(__file__).resolve().parents[1] / 'build' / 'zip')
    unzip_bin = os.environ.get('UNZIP_REWRITE_BIN')
    if unzip_bin is None:
        unzip_bin = str(Path(__file__).resolve().parents[1] / 'build' / 'unzip')

    size_gb = int(os.environ.get('ZU_LARGE_SIZE_GB', '5'))
    target_size = size_gb * 1024 * 1024 * 1024

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        data = tmp_path / 'large.bin'
        with open(data, 'wb') as f:
            f.truncate(target_size)

        archive = tmp_path / 'large.zip'
        res = run([zip_bin, str(archive), data.name], cwd=tmp_path)
        if res.returncode != 0:
            raise SystemExit(f"zip creation failed: {res.stderr or res.stdout}")

        expect_signatures(archive)

        test_res = run([unzip_bin, '-t', str(archive)], cwd=tmp_path)
        if test_res.returncode != 0:
            raise SystemExit(f"unzip -t failed: {test_res.stderr or test_res.stdout}")

        data.unlink()  # keep disk usage bounded
        out_dir = tmp_path / 'out'
        out_dir.mkdir()
        extract_res = run([unzip_bin, '-d', str(out_dir), str(archive)], cwd=tmp_path)
        if extract_res.returncode != 0:
            raise SystemExit(f"unzip extract failed: {extract_res.stderr or extract_res.stdout}")

        out_file = out_dir / 'large.bin'
        if not out_file.exists():
            raise SystemExit("extracted file missing")
        if out_file.stat().st_size != target_size:
            raise SystemExit(f"extracted size mismatch: expected {target_size}, got {out_file.stat().st_size}")


if __name__ == '__main__':
    main()
