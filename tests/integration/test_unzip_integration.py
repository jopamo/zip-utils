#!/usr/bin/env python3

import os
import stat
import subprocess
import tempfile
from pathlib import Path
from datetime import datetime

def run(cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

def test_attr_restoration(zip_bin, unzip_bin, tmp_path):
    test_file_name = 'perms_test.txt'
    test_file_path_src = tmp_path / 'src' / test_file_name
    original_content = "This file has special permissions and time."
    test_file_path_src.write_text(original_content)

    # Set specific permissions (e.g., rwx for owner, r-x for group, r-- for others)
    original_perms = 0o754
    os.chmod(test_file_path_src, original_perms)

    # Set a specific modification time (e.g., Jan 1, 2023, 10:30:00 UTC)
    original_mtime_dt = datetime(2023, 1, 1, 10, 30, 0)
    original_mtime_ts = int(original_mtime_dt.timestamp())
    os.utime(test_file_path_src, (original_mtime_ts, original_mtime_ts))

    return test_file_name, test_file_path_src, original_perms, original_mtime_ts, original_content

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

        test_file_name, test_file_path_src, original_perms, original_mtime_ts, original_content = test_attr_restoration(zip_bin, unzip_bin, tmp_path)

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
        extracted_test_file_path = dest / 'src' / test_file_name

        if not a_path.exists() or not b_path.exists() or not extracted_test_file_path.exists():
            raise SystemExit("extracted files missing")
        if a_path.read_text() != 'alpha\n' or b_path.read_text() != 'beta\n':
            raise SystemExit("extracted content mismatch")
        if extracted_test_file_path.read_text() != original_content:
            raise SystemExit("extracted test file content mismatch")

        # Verify permissions
        extracted_stat = os.stat(extracted_test_file_path)
        # We only care about the permission bits, mask out other bits like file type
        extracted_perms = stat.S_IMODE(extracted_stat.st_mode)
        if extracted_perms != original_perms:
            raise SystemExit(f"permission mismatch: expected {oct(original_perms)}, got {oct(extracted_perms)}")

        # Verify modification time
        # The FAT filesystem only stores 2-second resolution for modification time
        # So we compare with a 2-second tolerance
        if abs(extracted_stat.st_mtime - original_mtime_ts) > 1:
            raise SystemExit(f"modification time mismatch: expected {original_mtime_ts}, got {extracted_stat.st_mtime}")


if __name__ == '__main__':
    main()
