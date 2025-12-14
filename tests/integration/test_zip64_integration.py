#!/usr/bin/env python3

import os
import subprocess
import tempfile
from pathlib import Path
import zipfile


def run(cmd, cwd=None, env=None):
    return subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def parse_zip64_extra(extra):
    i = 0
    while i + 4 <= len(extra):
        header_id = int.from_bytes(extra[i:i + 2], 'little')
        data_len = int.from_bytes(extra[i + 2:i + 4], 'little')
        i += 4
        if i + data_len > len(extra):
            break
        if header_id == 0x0001:
            return True
        i += data_len
    return False


def has_signature(blob, signature):
    sig_bytes = signature.to_bytes(4, 'little')
    return sig_bytes in blob


def main():
    zip_bin = os.environ.get('WRITE_BIN')
    if zip_bin is None:
        zip_bin = str(Path(__file__).resolve().parents[1] / 'build' / 'zip')
    unzip_bin = os.environ.get('UNZIP_REWRITE_BIN')
    if unzip_bin is None:
        unzip_bin = os.environ.get('UNZIP_BIN')
    if unzip_bin is None:
        unzip_bin = str(Path(__file__).resolve().parents[1] / 'build' / 'unzip')

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        data_path = tmp_path / 'data.txt'
        data_path.write_text('zip64 tiny payload\n')

        archive = tmp_path / 'zip64.zip'
        env = os.environ.copy()
        env['ZU_TEST_ZIP64_TRIGGER'] = '1'
        res = run([zip_bin, str(archive), 'data.txt'], cwd=tmp_path, env=env)
        if res.returncode != 0:
            raise SystemExit(f"zip creation failed: {res.stderr}")

        blob = archive.read_bytes()
        if not (has_signature(blob, 0x06064B50) and has_signature(blob, 0x07064B50)):
            raise SystemExit("Zip64 EOCD or locator missing")

        with zipfile.ZipFile(archive, 'r') as zf:
            info = zf.getinfo('data.txt')
            if not parse_zip64_extra(info.extra):
                raise SystemExit("Zip64 extra missing from central header")
            if zf.read('data.txt').decode() != 'zip64 tiny payload\n':
                raise SystemExit("payload mismatch")

        test_res = run([unzip_bin, '-t', str(archive)])
        if test_res.returncode != 0:
            raise SystemExit(f"unzip -t failed: {test_res.stderr or test_res.stdout}")

        out_dir = tmp_path / 'out'
        out_dir.mkdir()
        extract_res = run([unzip_bin, '-d', str(out_dir), str(archive)])
        if extract_res.returncode != 0:
            raise SystemExit(f"unzip extract failed: {extract_res.stderr or extract_res.stdout}")
        if (out_dir / 'data.txt').read_text() != 'zip64 tiny payload\n':
            raise SystemExit("extracted payload mismatch")


if __name__ == '__main__':
    main()
