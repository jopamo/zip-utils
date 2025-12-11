#!/usr/bin/env python3

import os
import subprocess
import tempfile
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
        
        (tmp_path / 'f1.txt').write_text('f1')
        (tmp_path / 'f2.txt').write_text('f2')
        
        base_archive = tmp_path / 'base.zip'
        
        # Create base
        res = run([zip_bin, str(base_archive), 'f1.txt'], cwd=tmp_path)
        if res.returncode != 0:
            raise SystemExit(f"base zip failed: {res.stderr}")
            
        # Add f2 but output to out.zip
        out_archive = tmp_path / 'out.zip'
        cmd = [zip_bin, '-O', str(out_archive), str(base_archive), 'f2.txt']
        res = run(cmd, cwd=tmp_path)
        if res.returncode != 0:
            raise SystemExit(f"zip -O failed: {res.stderr}")
            
        if not out_archive.exists():
            raise SystemExit("Output file not created")
            
        # Verify base unmodified
        with zipfile.ZipFile(base_archive) as zf:
            names = zf.namelist()
            if names != ['f1.txt']:
                raise SystemExit(f"Base archive modified! Content: {names}")
                
        # Verify out has both
        with zipfile.ZipFile(out_archive) as zf:
            names = sorted(zf.namelist())
            if names != ['f1.txt', 'f2.txt']:
                raise SystemExit(f"Output archive incorrect! Content: {names}")

if __name__ == '__main__':
    main()
