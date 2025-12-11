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
        archive = work / 'quiet.zip'
        payload = work / 'a.txt'
        payload.write_text('quiet payload')

        create_res = run([zip_bin, str(archive), payload.name], cwd=work)
        if create_res.returncode != 0:
            raise SystemExit(f"zip create failed: {create_res.stderr or create_res.stdout}")

        base = run([unzip_bin, '-Z', str(archive)], cwd=work)
        if base.returncode != 0 or 'a.txt' not in base.stdout:
            raise SystemExit(f"baseline zipinfo missing entry: {base.stderr or base.stdout}")

        quiet_one = run([unzip_bin, '-Z', '-q', str(archive)], cwd=work)
        if quiet_one.returncode != 0:
            raise SystemExit(f"zipinfo -q failed: {quiet_one.stderr or quiet_one.stdout}")
        if quiet_one.stdout.strip():
            raise SystemExit(f"zipinfo -q produced output: {quiet_one.stdout}")

        quiet_two = run([unzip_bin, '-Z', '-qq', str(archive)], cwd=work)
        if quiet_two.returncode != 0:
            raise SystemExit(f"zipinfo -qq failed: {quiet_two.stderr or quiet_two.stdout}")
        if quiet_two.stdout.strip():
            raise SystemExit(f"zipinfo -qq produced output: {quiet_two.stdout}")

        pager = run([unzip_bin, '-Z', '-M', str(archive)], cwd=work)
        if pager.returncode != 0:
            raise SystemExit(f"zipinfo -M failed: {pager.stderr or pager.stdout}")
        if 'a.txt' not in pager.stdout:
            raise SystemExit(f"zipinfo -M missing entry output: {pager.stdout}")
        if '--More--' in pager.stdout or '--More--' in pager.stderr:
            raise SystemExit("pager prompt should not appear when stdout is not a TTY")


if __name__ == '__main__':
    main()
