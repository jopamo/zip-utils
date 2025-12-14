#!/usr/bin/env python3

import os
import subprocess
import tempfile
from pathlib import Path

def run(cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

def main():
    zip_bin = os.environ.get('WRITE_BIN')
    if zip_bin is None:
        zip_bin = str(Path(__file__).resolve().parents[1] / 'build' / 'zip')

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        (tmp_path / 'a.txt').write_text('content')
        
        log_file = tmp_path / 'test.log'
        archive = tmp_path / 'out.zip'
        
        # Test 1: Create log file
        cmd = [zip_bin, '-lf', str(log_file), '-li', str(archive), 'a.txt']
        res = run(cmd, cwd=tmp_path)
        if res.returncode != 0:
            raise SystemExit(f"zip failed: {res.stderr}")
            
        if not log_file.exists():
            raise SystemExit("Log file not created")
            
        content = log_file.read_text()
        if "adding: a.txt" not in content:
            raise SystemExit("Log missing entries")
            
        # Test 2: Append to log file
        (tmp_path / 'b.txt').write_text('content b')
        cmd = [zip_bin, '-la', '-lf', str(log_file), '-li', str(archive), 'b.txt']
        res = run(cmd, cwd=tmp_path)
        if res.returncode != 0:
            raise SystemExit(f"zip append failed: {res.stderr}")
            
        content = log_file.read_text()
        # Should have both a.txt (from before) and b.txt
        if "adding: a.txt" not in content:
            raise SystemExit("Log lost previous content (append failed)")
        if "adding: b.txt" not in content:
            raise SystemExit("Log missing new entry")

if __name__ == '__main__':
    main()
