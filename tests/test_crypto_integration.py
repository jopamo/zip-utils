#!/usr/bin/env python3

import os
import subprocess
import tempfile
from pathlib import Path
import zipfile

def run(cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

def main():
    zip_bin = os.environ.get('WRITE_BIN')
    if zip_bin is None:
        # Fallback for manual run
        zip_bin = str(Path(__file__).resolve().parents[1] / 'build' / 'zip')
    
    unzip_rewrite_bin = os.environ.get('UNZIP_REWRITE_BIN')
    if unzip_rewrite_bin is None:
        # Fallback for manual run
        unzip_rewrite_bin = str(Path(__file__).resolve().parents[1] / 'build' / 'unzip')

    if not os.path.exists(zip_bin) or not os.path.exists(unzip_rewrite_bin):
         print(f"Skipping crypto test: binaries not found at {zip_bin} / {unzip_rewrite_bin}")
         return

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        payload = tmp_path / 'secret.txt'
        payload.write_text('Top Secret')
        
        archive = tmp_path / 'encrypted.zip'
        
        # Create encrypted zip
        pwd = 'mypassword'
        # Note: zip stores path relative to CWD.
        create = run([zip_bin, '-e', '-P', pwd, str(archive), 'secret.txt'], cwd=tmp_path)
        if create.returncode != 0:
            raise SystemExit(f"zip creation failed: {create.stderr}")
            
        # Verify with Python zipfile
        try:
            with zipfile.ZipFile(archive, 'r') as zf:
                info = zf.getinfo('secret.txt')
                if not (info.flag_bits & 0x1):
                    raise SystemExit("File not flagged as encrypted")
                
                zf.setpassword(pwd.encode())
                content = zf.read('secret.txt').decode()
                if content != 'Top Secret':
                    raise SystemExit(f"Python extraction content mismatch: {content}")
        except Exception as e:
            raise SystemExit(f"Python zipfile validation failed: {e}")

        # Verify with our unzip
        extract = run([unzip_rewrite_bin, '-p', '-P', pwd, str(archive)], cwd=tmp_path)
        if extract.returncode != 0:
             raise SystemExit(f"unzip failed: {extract.stderr}")
        if extract.stdout != 'Top Secret':
             raise SystemExit(f"unzip content mismatch: got '{extract.stdout}'")

        # Verify failure with wrong password
        fail = run([unzip_rewrite_bin, '-p', '-P', 'wrong', str(archive)], cwd=tmp_path)
        if fail.returncode == 0:
             raise SystemExit("unzip succeeded with wrong password")

    print("Crypto integration test passed.")

if __name__ == '__main__':
    main()
