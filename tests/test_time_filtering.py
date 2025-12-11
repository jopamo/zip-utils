#!/usr/bin/env python3

import os
import subprocess
import tempfile
import time
import zipfile
from pathlib import Path

def run(cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

def main():
    zip_bin = os.environ.get('WRITE_BIN')
    if zip_bin is None:
        zip_bin = str(Path(__file__).resolve().parents[1] / 'build' / 'zip')

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        
        f1 = tmp_path / 'old.txt'
        f1.write_text('old')
        t1 = time.mktime(time.strptime("2020-01-01", "%Y-%m-%d"))
        os.utime(f1, (t1, t1))
        
        f2 = tmp_path / 'new.txt'
        f2.write_text('new')
        t2 = time.mktime(time.strptime("2025-01-01", "%Y-%m-%d"))
        os.utime(f2, (t2, t2))
        
        # Test -t (After)
        # Should include new.txt, exclude old.txt
        archive_after = tmp_path / 'after.zip'
        cmd = [zip_bin, '-t', '2021-01-01', str(archive_after), 'old.txt', 'new.txt']
        res = run(cmd, cwd=tmp_path)
        if res.returncode != 0:
            raise SystemExit(f"zip -t failed: {res.stderr}")
            
        with zipfile.ZipFile(archive_after) as zf:
            names = zf.namelist()
            if 'new.txt' not in names:
                raise SystemExit("Failed: new.txt missing from -t archive")
            if 'old.txt' in names:
                raise SystemExit("Failed: old.txt included in -t archive")
                
        # Test -tt (Before)
        # Should include old.txt, exclude new.txt
        archive_before = tmp_path / 'before.zip'
        cmd = [zip_bin, '-tt', '2021-01-01', str(archive_before), 'old.txt', 'new.txt']
        res = run(cmd, cwd=tmp_path)
        if res.returncode != 0:
            raise SystemExit(f"zip -tt failed: {res.stderr}")
            
        with zipfile.ZipFile(archive_before) as zf:
            names = zf.namelist()
            if 'old.txt' not in names:
                raise SystemExit("Failed: old.txt missing from -tt archive")
            if 'new.txt' in names:
                raise SystemExit("Failed: new.txt included in -tt archive")

if __name__ == '__main__':
    main()
